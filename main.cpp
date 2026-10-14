#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#include "portable-file-dialogs.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include "ImGuizmo.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <chrono>
#include <reactphysics3d/reactphysics3d.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "stb_image.h"

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// This is our first tiny "component": every scene object has a display name
// and a transform. Rotation values are stored as degrees because that is much
// friendlier to edit in the Inspector than radians.
class Model;

struct Entity {
    std::string name;
    glm::vec3 position{0.0F, 0.0F, 0.0F};
    glm::vec3 rotation{0.0F, 0.0F, 0.0F};
    glm::vec3 scale{1.0F, 1.0F, 1.0F};
    bool isActive = true;
    bool isDynamic = false;
    std::uint16_t collisionCategory = 0x0001U;
    std::uint16_t collisionMask = 0xFFFFU;

    // ReactPhysics3D owns these objects. They must be destroyed through the
    // PhysicsWorld/PhysicsCommon factories instead of with delete.
    reactphysics3d::RigidBody* rigidBody = nullptr;
    reactphysics3d::BoxShape* collisionShape = nullptr;
    reactphysics3d::Collider* collider = nullptr;

    std::shared_ptr<Model> model;
    std::string modelPath;
};

// For now, a contiguous vector is the entire scene/world database. This is
// cache-friendly, simple to inspect, and more than adequate for a small level.
std::vector<Entity> sceneEntities;
int selectedEntityIndex = -1;

struct CameraState {
    glm::vec3 cameraPos{0.0F, 0.0F, 3.0F};
    glm::vec3 cameraFront{0.0F, 0.0F, -1.0F};
    glm::vec3 cameraUp{0.0F, 1.0F, 0.0F};

    float yaw = -90.0F;
    float pitch = 0.0F;
    float movementSpeed = 2.5F;
    float mouseSensitivity = 0.1F;
};

// OpenGL 3.1 corresponds to GLSL 1.40. GLSL 1.40 does not have the modern
// core layout(location = ...) syntax, so attribute locations are assigned in
// initializeRenderer() immediately before the shader program is linked.
constexpr const char* kVertexShaderSource = R"GLSL(
#version 140

in vec3 aPosition;
in vec3 aNormal;
in vec2 aTexCoords;

out vec2 TexCoords;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

void main()
{
    // Matrix multiplication reads right-to-left:
    // model/local space -> world space -> camera space -> clip space.
    gl_Position = uProjection * uView * uModel * vec4(aPosition, 1.0);
    TexCoords = aTexCoords;
}
)GLSL";

constexpr const char* kFragmentShaderSource = R"GLSL(
#version 140

in vec2 TexCoords;
out vec4 fragmentColor;

uniform sampler2D uDiffuseTexture;

void main()
{
    fragmentColor = texture(uDiffuseTexture, TexCoords);
}
)GLSL";

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 textureCoordinates;
};

GLuint createWhiteTexture()
{
    constexpr unsigned char whitePixel[] = {255, 255, 255, 255};
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, whitePixel);
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

// Loads PNG/JPG/etc. pixel data with stb_image and uploads it to OpenGL.
// Nearest-neighbor filtering deliberately preserves the crunchy PS2 look.
GLuint loadTextureFromFile(const std::filesystem::path& path)
{
    stbi_set_flip_vertically_on_load(1);

    int width = 0;
    int height = 0;
    int channelCount = 0;
    unsigned char* pixels =
        stbi_load(path.string().c_str(), &width, &height, &channelCount, 0);

    if (pixels == nullptr) {
        std::cerr << "Failed to load texture '" << path.string()
                  << "': " << stbi_failure_reason() << '\n';
        return 0;
    }

    GLenum format = GL_RGBA;
    if (channelCount == 1) {
        format = GL_RED;
    } else if (channelCount == 2) {
        format = GL_RG;
    } else if (channelCount == 3) {
        format = GL_RGB;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // OBJ textures are not guaranteed to have rows aligned to four bytes.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(
        GL_TEXTURE_2D, 0, static_cast<GLint>(format), width, height, 0,
        format, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);
    return texture;
}

class Mesh final {
public:
    Mesh(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices)
    {
        setup(vertices, indices);
    }

    ~Mesh()
    {
        release();
    }

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    Mesh(Mesh&& other) noexcept
    {
        takeOwnership(other);
    }

    Mesh& operator=(Mesh&& other) noexcept
    {
        if (this != &other) {
            release();
            takeOwnership(other);
        }
        return *this;
    }

    void Draw() const
    {
        glBindVertexArray(vao_);
        glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);
    }

private:
    void setup(
        const std::vector<Vertex>& vertices,
        const std::vector<unsigned int>& indices)
    {
        indexCount_ = static_cast<GLsizei>(indices.size());

        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glGenBuffers(1, &ebo_);

        glBindVertexArray(vao_);

        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(
            GL_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
            vertices.data(),
            GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
        glBufferData(
            GL_ELEMENT_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(indices.size() * sizeof(unsigned int)),
            indices.data(),
            GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(
            0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
            reinterpret_cast<const void*>(offsetof(Vertex, position)));

        glEnableVertexAttribArray(1);
        glVertexAttribPointer(
            1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
            reinterpret_cast<const void*>(offsetof(Vertex, normal)));

        glEnableVertexAttribArray(2);
        glVertexAttribPointer(
            2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
            reinterpret_cast<const void*>(offsetof(Vertex, textureCoordinates)));

        // Do not unbind the EBO separately: its binding belongs to the VAO.
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    void release() noexcept
    {
        if (ebo_ != 0) {
            glDeleteBuffers(1, &ebo_);
        }
        if (vbo_ != 0) {
            glDeleteBuffers(1, &vbo_);
        }
        if (vao_ != 0) {
            glDeleteVertexArrays(1, &vao_);
        }
        vao_ = 0;
        vbo_ = 0;
        ebo_ = 0;
        indexCount_ = 0;
    }

    void takeOwnership(Mesh& other) noexcept
    {
        vao_ = other.vao_;
        vbo_ = other.vbo_;
        ebo_ = other.ebo_;
        indexCount_ = other.indexCount_;
        other.vao_ = 0;
        other.vbo_ = 0;
        other.ebo_ = 0;
        other.indexCount_ = 0;
    }

    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ebo_ = 0;
    GLsizei indexCount_ = 0;
};

class Model final {
public:
    explicit Model(const std::filesystem::path& path)
    {
        load(path);
    }

    // Procedural geometry uses the same tiny Model/Mesh draw path as imported
    // assets, but avoids Assimp and disk I/O entirely.
    Model(
        const std::vector<Vertex>& vertices,
        const std::vector<unsigned int>& indices)
    {
        meshes_.emplace_back(vertices, indices);
        texture_ = createWhiteTexture();
    }

    ~Model()
    {
        if (texture_ != 0) {
            glDeleteTextures(1, &texture_);
        }
    }

    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    void BindTexture() const
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture_);
    }

    void Draw() const
    {
        for (const Mesh& mesh : meshes_) {
            mesh.Draw();
        }
    }

private:
    void load(const std::filesystem::path& path)
    {
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(
            path.string(),
            aiProcess_Triangulate |
            aiProcess_GenSmoothNormals |
            aiProcess_JoinIdenticalVertices);

        if (scene == nullptr ||
            (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0U ||
            scene->mRootNode == nullptr) {
            throw std::runtime_error(
                "Assimp could not load '" + path.string() + "': " +
                importer.GetErrorString());
        }

        directory_ = path.parent_path();
        processNode(scene->mRootNode, scene);

        if (meshes_.empty()) {
            throw std::runtime_error("The model contains no renderable meshes");
        }

        // Models without a diffuse map remain visible instead of sampling an
        // undefined texture. The unlit shader displays this as solid white.
        if (texture_ == 0) {
            texture_ = createWhiteTexture();
        }
    }

    void processNode(aiNode* node, const aiScene* scene)
    {
        for (unsigned int index = 0; index < node->mNumMeshes; ++index) {
            aiMesh* mesh = scene->mMeshes[node->mMeshes[index]];
            processMesh(mesh, scene);
        }

        for (unsigned int index = 0; index < node->mNumChildren; ++index) {
            processNode(node->mChildren[index], scene);
        }
    }

    void processMesh(aiMesh* mesh, const aiScene* scene)
    {
        std::vector<Vertex> vertices;
        std::vector<unsigned int> indices;
        vertices.reserve(mesh->mNumVertices);

        for (unsigned int index = 0; index < mesh->mNumVertices; ++index) {
            Vertex vertex{};
            vertex.position = {
                mesh->mVertices[index].x,
                mesh->mVertices[index].y,
                mesh->mVertices[index].z};

            if (mesh->HasNormals()) {
                vertex.normal = {
                    mesh->mNormals[index].x,
                    mesh->mNormals[index].y,
                    mesh->mNormals[index].z};
            }

            if (mesh->mTextureCoords[0] != nullptr) {
                vertex.textureCoordinates = {
                    mesh->mTextureCoords[0][index].x,
                    mesh->mTextureCoords[0][index].y};
            }

            vertices.push_back(vertex);
        }

        for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
            const aiFace& face = mesh->mFaces[faceIndex];
            for (unsigned int index = 0; index < face.mNumIndices; ++index) {
                indices.push_back(face.mIndices[index]);
            }
        }

        // This lightweight first pass uses one diffuse texture for the model.
        // The first material texture found is resolved relative to the OBJ.
        if (texture_ == 0 && mesh->mMaterialIndex < scene->mNumMaterials) {
            const aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
            aiString relativeTexturePath;

            if (material->GetTexture(
                    aiTextureType_DIFFUSE, 0, &relativeTexturePath) == AI_SUCCESS) {
                const std::string relativePath = relativeTexturePath.C_Str();

                // Embedded textures use names such as "*0". Keeping this
                // starter focused on external PNG/JPG files avoids extra CPU
                // decoding paths and is ideal for editable OBJ asset folders.
                if (!relativePath.empty() && relativePath.front() != '*') {
                    texture_ = loadTextureFromFile(
                        directory_ / std::filesystem::u8path(relativePath));
                }
            }
        }

        meshes_.emplace_back(vertices, indices);
    }

    std::vector<Mesh> meshes_;
    std::filesystem::path directory_;
    GLuint texture_ = 0;
};

std::shared_ptr<Model> createDefaultCubeModel()
{
    // A centered unit cube. The current shader is unlit, so shared corner
    // vertices are sufficient and keep this primitive extremely small.
    const std::vector<Vertex> vertices = {
        {{-0.5F, -0.5F, -0.5F}, {0.0F, 0.0F, -1.0F}, {0.0F, 0.0F}},
        {{ 0.5F, -0.5F, -0.5F}, {0.0F, 0.0F, -1.0F}, {1.0F, 0.0F}},
        {{ 0.5F,  0.5F, -0.5F}, {0.0F, 0.0F, -1.0F}, {1.0F, 1.0F}},
        {{-0.5F,  0.5F, -0.5F}, {0.0F, 0.0F, -1.0F}, {0.0F, 1.0F}},
        {{-0.5F, -0.5F,  0.5F}, {0.0F, 0.0F,  1.0F}, {0.0F, 0.0F}},
        {{ 0.5F, -0.5F,  0.5F}, {0.0F, 0.0F,  1.0F}, {1.0F, 0.0F}},
        {{ 0.5F,  0.5F,  0.5F}, {0.0F, 0.0F,  1.0F}, {1.0F, 1.0F}},
        {{-0.5F,  0.5F,  0.5F}, {0.0F, 0.0F,  1.0F}, {0.0F, 1.0F}}
    };

    const std::vector<unsigned int> indices = {
        0, 2, 1, 0, 3, 2, // back
        4, 5, 6, 4, 6, 7, // front
        0, 4, 7, 0, 7, 3, // left
        1, 2, 6, 1, 6, 5, // right
        0, 1, 5, 0, 5, 4, // bottom
        3, 7, 6, 3, 6, 2  // top
    };

    return std::make_shared<Model>(vertices, indices);
}

void glfwErrorCallback(int error, const char* description)
{
    std::cerr << "GLFW error " << error << ": " << description << '\n';
}

class Application final {
public:
    Application()
    {
        try {
            initializeWindow();
            initializeRenderer();
            initializePhysics();
            initializeUI();
        } catch (...) {
            shutdown();
            throw;
        }
    }

    ~Application()
    {
        shutdown();
    }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void run()
    {
        lastFrame_ = static_cast<float>(glfwGetTime());

        while (!glfwWindowShouldClose(window_)) {
            // DELTA TIME ----------------------------------------------------
            // Movement below is measured in units per second, rather than
            // units per frame, so a fast GPU does not move the camera faster.
            const float currentFrame = static_cast<float>(glfwGetTime());
            deltaTime_ = currentFrame - lastFrame_;
            lastFrame_ = currentFrame;

            // 1. Poll keyboard and window events.
            pollInputs();

            // 2. Advance engine timing/state.
            updateEngineState();
            updatePhysics();

            // 3. Draw the 3D world, then draw ImGui over it.
            renderFrame();

            // 4. Present the completed frame. VSync limits this call.
            glfwSwapBuffers(window_);
        }
    }

private:
    void initializeWindow()
    {
        glfwSetErrorCallback(glfwErrorCallback);

        if (glfwInit() != GLFW_TRUE) {
            throw std::runtime_error("Failed to initialize GLFW");
        }
        glfwInitialized_ = true;

        // Named core/compatibility profiles were introduced in OpenGL 3.2.
        // Requesting GLFW_OPENGL_CORE_PROFILE with version 3.1 is invalid, so
        // a 3.1 forward-compatible context is the strict non-deprecated form.
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        glfwWindowHint(GLFW_DEPTH_BITS, 24);

        window_ = glfwCreateWindow(1280, 720, "PotatoEngine", nullptr, nullptr);
        if (window_ == nullptr) {
            throw std::runtime_error("Failed to create the GLFW window");
        }

        glfwMakeContextCurrent(window_);
        glfwSwapInterval(1);

        // The static callback recovers this Application through GLFW's user
        // pointer. It is registered before ImGui, allowing ImGui's backend to
        // preserve and chain it when installing its own input callbacks.
        glfwSetWindowUserPointer(window_, this);
        glfwSetCursorPosCallback(window_, cursorPositionCallback);

        if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
            throw std::runtime_error("Failed to load OpenGL functions with GLAD");
        }
    }

    static GLuint compileShader(GLenum type, const char* source)
    {
        const GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);

        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled == GL_TRUE) {
            return shader;
        }

        // Surface the driver's shader compiler output instead of failing with
        // an unexplained black screen.
        GLint logLength = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<std::size_t>(logLength), '\0');
        glGetShaderInfoLog(shader, logLength, nullptr, log.data());
        glDeleteShader(shader);
        throw std::runtime_error("Shader compilation failed:\n" + log);
    }

    void initializeRenderer()
    {
        GLuint vertexShader = 0;
        GLuint fragmentShader = 0;

        try {
            vertexShader = compileShader(GL_VERTEX_SHADER, kVertexShaderSource);
            fragmentShader = compileShader(GL_FRAGMENT_SHADER, kFragmentShaderSource);

            shaderProgram_ = glCreateProgram();
            glAttachShader(shaderProgram_, vertexShader);
            glAttachShader(shaderProgram_, fragmentShader);

            // GLSL 1.40 has no core layout(location) qualifier. Establish the
            // Mesh VAO contract explicitly before linking.
            glBindAttribLocation(shaderProgram_, 0, "aPosition");
            glBindAttribLocation(shaderProgram_, 1, "aNormal");
            glBindAttribLocation(shaderProgram_, 2, "aTexCoords");
            glLinkProgram(shaderProgram_);

            GLint linked = GL_FALSE;
            glGetProgramiv(shaderProgram_, GL_LINK_STATUS, &linked);
            if (linked != GL_TRUE) {
                GLint logLength = 0;
                glGetProgramiv(shaderProgram_, GL_INFO_LOG_LENGTH, &logLength);
                std::string log(static_cast<std::size_t>(logLength), '\0');
                glGetProgramInfoLog(shaderProgram_, logLength, nullptr, log.data());
                throw std::runtime_error("Shader program linking failed:\n" + log);
            }
        } catch (...) {
            if (vertexShader != 0) {
                glDeleteShader(vertexShader);
            }
            if (fragmentShader != 0) {
                glDeleteShader(fragmentShader);
            }
            throw;
        }

        // The linked program keeps the compiled code; separate shader objects
        // are no longer necessary.
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);

        modelLocation_ = glGetUniformLocation(shaderProgram_, "uModel");
        viewLocation_ = glGetUniformLocation(shaderProgram_, "uView");
        projectionLocation_ = glGetUniformLocation(shaderProgram_, "uProjection");
        textureLocation_ = glGetUniformLocation(shaderProgram_, "uDiffuseTexture");
        if (modelLocation_ < 0 || viewLocation_ < 0 ||
            projectionLocation_ < 0 || textureLocation_ < 0) {
            throw std::runtime_error("Failed to locate a required shader uniform");
        }

        // Every Model binds its texture on unit zero before drawing.
        glUseProgram(shaderProgram_);
        glUniform1i(textureLocation_, 0);
        glUseProgram(0);

        // CAMERA MATH -------------------------------------------------------
        // The projection is static for this fixed-aspect window. The view is
        // rebuilt every frame later because the editor camera can move.
        projection_ = glm::perspective(
            glm::radians(45.0F),
            1280.0F / 720.0F,
            0.1F,
            100.0F);

    }

    void initializePhysics()
    {
        // PhysicsCommon is ReactPhysics3D's allocator/factory. Keep it alive
        // longer than the world, bodies, and collision shapes it creates.
        reactphysics3d::PhysicsWorld::WorldSettings settings;
        settings.gravity = reactphysics3d::Vector3(0.0F, -9.81F, 0.0F);
        physicsWorld_ = physicsCommon_.createPhysicsWorld(settings);

        if (physicsWorld_ == nullptr) {
            throw std::runtime_error("Failed to create the physics world");
        }
    }

    void initializeUI()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        imguiContextCreated_ = true;
        ImGui::StyleColorsDark();

        if (!ImGui_ImplGlfw_InitForOpenGL(window_, true)) {
            throw std::runtime_error("Failed to initialize the ImGui GLFW backend");
        }
        imguiGlfwInitialized_ = true;

        if (!ImGui_ImplOpenGL3_Init("#version 140")) {
            throw std::runtime_error("Failed to initialize the ImGui OpenGL backend");
        }
        imguiOpenGLInitialized_ = true;
    }

    void pollInputs()
    {
        glfwPollEvents();

        if (glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window_, GLFW_TRUE);
        }

        // RMB toggles editor navigation mode. A disabled cursor is hidden and
        // unconstrained, so the user can keep turning without hitting an edge.
        // Releasing RMB restores the normal cursor for ImGui interaction.
        const bool rightMouseDown =
            glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

        if (rightMouseDown && !mouseLookActive_) {
            mouseLookActive_ = true;
            firstMouseSample_ = true;
            glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        } else if (!rightMouseDown && mouseLookActive_) {
            mouseLookActive_ = false;
            firstMouseSample_ = true;
            glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
    }

    void updateEngineState()
    {
        // Editor movement is active only while RMB is held. This leaves WASD
        // available to future UI text fields and editor shortcuts otherwise.
        if (!mouseLookActive_) {
            return;
        }

        const float movement = camera_.movementSpeed * deltaTime_;
        const glm::vec3 cameraRight =
            glm::normalize(glm::cross(camera_.cameraFront, camera_.cameraUp));

        if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS) {
            camera_.cameraPos += camera_.cameraFront * movement;
        }
        if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS) {
            camera_.cameraPos -= camera_.cameraFront * movement;
        }
        if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS) {
            camera_.cameraPos -= cameraRight * movement;
        }
        if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS) {
            camera_.cameraPos += cameraRight * movement;
        }
        if (glfwGetKey(window_, GLFW_KEY_E) == GLFW_PRESS) {
            camera_.cameraPos += camera_.cameraUp * movement;
        }
        if (glfwGetKey(window_, GLFW_KEY_Q) == GLFW_PRESS) {
            camera_.cameraPos -= camera_.cameraUp * movement;
        }
    }

    static glm::mat4 makeModelMatrix(const Entity& entity)
    {
        glm::mat4 model = glm::translate(glm::mat4(1.0F), entity.position);
        model = glm::rotate(
            model, glm::radians(entity.rotation.z), glm::vec3(0.0F, 0.0F, 1.0F));
        model = glm::rotate(
            model, glm::radians(entity.rotation.y), glm::vec3(0.0F, 1.0F, 0.0F));
        model = glm::rotate(
            model, glm::radians(entity.rotation.x), glm::vec3(1.0F, 0.0F, 0.0F));
        return glm::scale(model, entity.scale);
    }

    static reactphysics3d::Transform makePhysicsTransform(const Entity& entity)
    {
        const reactphysics3d::Vector3 position(
            entity.position.x, entity.position.y, entity.position.z);
        const reactphysics3d::Quaternion orientation =
            reactphysics3d::Quaternion::fromEulerAngles(
                glm::radians(entity.rotation.x),
                glm::radians(entity.rotation.y),
                glm::radians(entity.rotation.z));
        return reactphysics3d::Transform(position, orientation);
    }

    static reactphysics3d::Vector3 makeColliderHalfExtents(const glm::vec3& scale)
    {
        // A minimum extent prevents invalid zero-volume shapes if an Inspector
        // scale component is dragged to zero. The box represents a unit model,
        // so its half extents are one half of the visual scale.
        constexpr float minimumHalfExtent = 0.01F;
        const glm::vec3 halfExtents = glm::max(
            glm::abs(scale) * 0.5F, glm::vec3(minimumHalfExtent));
        return reactphysics3d::Vector3(
            halfExtents.x, halfExtents.y, halfExtents.z);
    }

    void createPhysicsBody(Entity& entity)
    {
        if (physicsWorld_ == nullptr || entity.rigidBody != nullptr) {
            return;
        }

        entity.collisionShape =
            physicsCommon_.createBoxShape(makeColliderHalfExtents(entity.scale));
        entity.rigidBody =
            physicsWorld_->createRigidBody(makePhysicsTransform(entity));
        entity.collider = entity.rigidBody->addCollider(
            entity.collisionShape, reactphysics3d::Transform::identity());
        entity.collider->setCollisionCategoryBits(entity.collisionCategory);
        entity.collider->setCollideWithMaskBits(entity.collisionMask);
        entity.rigidBody->setType(
            entity.isDynamic ? reactphysics3d::BodyType::DYNAMIC
                             : reactphysics3d::BodyType::STATIC);
        entity.rigidBody->setIsActive(entity.isActive);

        if (entity.isDynamic) {
            entity.rigidBody->updateMassPropertiesFromColliders();
        }
    }

    void destroyPhysicsBody(Entity& entity) noexcept
    {
        if (physicsWorld_ != nullptr && entity.rigidBody != nullptr) {
            physicsWorld_->destroyRigidBody(entity.rigidBody);
            entity.rigidBody = nullptr;
        }
        // Colliders are owned by their rigid body and become invalid as soon
        // as the body is destroyed, regardless of who initiated destruction.
        entity.collider = nullptr;
        if (entity.collisionShape != nullptr) {
            physicsCommon_.destroyBoxShape(entity.collisionShape);
            entity.collisionShape = nullptr;
        }
    }

    void updatePhysics()
    {
        if (physicsWorld_ == nullptr) {
            return;
        }

        // A native file dialog can pause the main thread for several seconds.
        // Clamp that one unusually large frame so it cannot explode the
        // simulation when the dialog closes. A production engine should move
        // to a fixed-timestep accumulator once deterministic physics matters.
        const float physicsDeltaTime = std::min(deltaTime_, 1.0F / 30.0F);
        if (physicsDeltaTime > 0.0F) {
            physicsWorld_->update(
                static_cast<reactphysics3d::decimal>(physicsDeltaTime));
        }

        // Physics is authoritative. Copy each active body's new transform back
        // into the simple editor transform used by the OpenGL render loop.
        // The Kill Z check happens before anything reaches GLM, preventing a
        // runaway body (or an already-corrupt transform) from poisoning the
        // model matrix with huge values or NaNs.
        for (Entity& entity : sceneEntities) {
            if (!entity.isActive || entity.rigidBody == nullptr) {
                continue;
            }

            const reactphysics3d::Transform& transform =
                entity.rigidBody->getTransform();
            const reactphysics3d::Vector3& position = transform.getPosition();
            const reactphysics3d::Quaternion& orientation =
                transform.getOrientation();

            constexpr reactphysics3d::decimal killZ = -500.0F;
            const bool invalidTransform =
                !std::isfinite(position.x) || !std::isfinite(position.y) ||
                !std::isfinite(position.z) || !std::isfinite(orientation.x) ||
                !std::isfinite(orientation.y) || !std::isfinite(orientation.z) ||
                !std::isfinite(orientation.w);

            if (position.y < killZ || invalidTransform) {
                // Destroy the body instead of teleporting or deactivating it.
                // ReactPhysics3D can then remove its broad-phase/contact data,
                // while the null pointer prevents every later dereference.
                if (entity.rigidBody != nullptr) {
                    physicsWorld_->destroyRigidBody(entity.rigidBody);
                    entity.rigidBody = nullptr;

                    // The collider is body-owned and was destroyed with it.
                    // Clear our non-owning pointer at the same time.
                    entity.collider = nullptr;
                }
                entity.isActive = false;
                continue;
            }

            entity.position = glm::vec3(position.x, position.y, position.z);
            const glm::quat glmOrientation(
                orientation.w, orientation.x, orientation.y, orientation.z);
            entity.rotation = glm::degrees(glm::eulerAngles(glmOrientation));
        }
    }

    static void cursorPositionCallback(GLFWwindow* window, double xPosition, double yPosition)
    {
        auto* application = static_cast<Application*>(glfwGetWindowUserPointer(window));
        if (application != nullptr) {
            application->handleMouseMovement(xPosition, yPosition);
        }
    }

    void handleMouseMovement(double xPosition, double yPosition)
    {
        // Check the physical button state as well as our mode flag. This
        // guarantees that ordinary cursor movement over ImGui never rotates
        // the camera, even during a press/release transition.
        if (!mouseLookActive_ ||
            glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT) != GLFW_PRESS) {
            firstMouseSample_ = true;
            return;
        }

        const float currentX = static_cast<float>(xPosition);
        const float currentY = static_cast<float>(yPosition);

        // Discard the first sample after capture. Otherwise the jump from the
        // visible cursor position to disabled mode could snap the camera.
        if (firstMouseSample_) {
            lastMouseX_ = currentX;
            lastMouseY_ = currentY;
            firstMouseSample_ = false;
            return;
        }

        float xOffset = currentX - lastMouseX_;
        float yOffset = lastMouseY_ - currentY; // Window Y grows downward.
        lastMouseX_ = currentX;
        lastMouseY_ = currentY;

        xOffset *= camera_.mouseSensitivity;
        yOffset *= camera_.mouseSensitivity;
        camera_.yaw += xOffset;
        camera_.pitch += yOffset;
        camera_.pitch = std::clamp(camera_.pitch, -89.0F, 89.0F);

        // Convert yaw and pitch angles into a normalized forward direction.
        glm::vec3 direction;
        direction.x =
            std::cos(glm::radians(camera_.yaw)) * std::cos(glm::radians(camera_.pitch));
        direction.y = std::sin(glm::radians(camera_.pitch));
        direction.z =
            std::sin(glm::radians(camera_.yaw)) * std::cos(glm::radians(camera_.pitch));
        camera_.cameraFront = glm::normalize(direction);
    }

    void renderScene()
    {
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(window_, &framebufferWidth, &framebufferHeight);
        glViewport(0, 0, framebufferWidth, framebufferHeight);

        // WORLD RENDERING ---------------------------------------------------
        // Clear both buffers. Without the depth buffer, rear cube triangles
        // could be drawn on top of triangles that are closer to the camera.
        glEnable(GL_DEPTH_TEST);
        glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // MODEL MATH --------------------------------------------------------
        // Select the shared cube shader. View and projection are identical for
        // every entity, so upload them only once for the whole scene.
        // Rebuild the view every frame because the editor camera can move.
        const glm::mat4 view = glm::lookAt(
            camera_.cameraPos,
            camera_.cameraPos + camera_.cameraFront,
            camera_.cameraUp);

        glUseProgram(shaderProgram_);
        glUniformMatrix4fv(viewLocation_, 1, GL_FALSE, glm::value_ptr(view));
        glUniformMatrix4fv(projectionLocation_, 1, GL_FALSE, glm::value_ptr(projection_));

        for (const Entity& entity : sceneEntities) {
            // Inactive entities remain in the scene and Inspector, but consume
            // no transform work, texture binding, or draw calls.
            if (!entity.isActive) {
                continue;
            }

            if (!entity.model) {
                continue;
            }

            // MODEL MATH ----------------------------------------------------
            // The same helper is used by rendering and ImGuizmo, keeping the
            // selected gizmo perfectly aligned with the drawn object.
            const glm::mat4 model = makeModelMatrix(entity);

            // GL_FALSE is correct because GLM uses OpenGL's column-major
            // matrix layout. Each entity then draws all of its imported meshes.
            glUniformMatrix4fv(modelLocation_, 1, GL_FALSE, glm::value_ptr(model));
            entity.model->BindTexture();
            entity.model->Draw();
        }

        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
    }

    void renderTransformGizmo()
    {
        const bool hasValidSelection =
            selectedEntityIndex >= 0 &&
            static_cast<std::size_t>(selectedEntityIndex) < sceneEntities.size();
        if (!hasValidSelection) {
            return;
        }

        Entity& entity =
            sceneEntities[static_cast<std::size_t>(selectedEntityIndex)];
        if (!entity.isActive) {
            return;
        }

        const glm::mat4 view = glm::lookAt(
            camera_.cameraPos,
            camera_.cameraPos + camera_.cameraFront,
            camera_.cameraUp);
        glm::mat4 model = makeModelMatrix(entity);

        const ImGuiIO& io = ImGui::GetIO();
        ImGuizmo::SetOrthographic(false);
        // Background draw data places the gizmo above OpenGL but below editor
        // windows, so it cannot paint over the Hierarchy or Inspector.
        ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
        ImGuizmo::SetRect(0.0F, 0.0F, io.DisplaySize.x, io.DisplaySize.y);
        ImGuizmo::Manipulate(
            glm::value_ptr(view),
            glm::value_ptr(projection_),
            gizmoOperation_,
            gizmoMode_,
            glm::value_ptr(model));

        if (!ImGuizmo::IsUsing()) {
            return;
        }

        float translation[3]{};
        float rotation[3]{};
        float scale[3]{};
        ImGuizmo::DecomposeMatrixToComponents(
            glm::value_ptr(model), translation, rotation, scale);

        entity.position = glm::make_vec3(translation);
        entity.rotation = glm::make_vec3(rotation);
        entity.scale = glm::make_vec3(scale);

        if (entity.collisionShape != nullptr) {
            entity.collisionShape->setHalfExtents(
                makeColliderHalfExtents(entity.scale));
        }
        if (entity.rigidBody != nullptr) {
            entity.rigidBody->setTransform(makePhysicsTransform(entity));
            entity.rigidBody->setIsSleeping(false);
            if (entity.isDynamic) {
                // Editor manipulation is a teleport, not a physical impulse.
                entity.rigidBody->setLinearVelocity(
                    reactphysics3d::Vector3(0.0F, 0.0F, 0.0F));
                entity.rigidBody->setAngularVelocity(
                    reactphysics3d::Vector3(0.0F, 0.0F, 0.0F));
                entity.rigidBody->updateMassPropertiesFromColliders();
            }
        }
    }

    static float getCurrentRamUsageMB() noexcept
    {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS processMemory{};
        processMemory.cb = sizeof(processMemory);

        if (GetProcessMemoryInfo(
                GetCurrentProcess(), &processMemory, sizeof(processMemory)) == FALSE) {
            return 0.0F;
        }

        constexpr double bytesPerMegabyte = 1024.0 * 1024.0;
        return static_cast<float>(
            static_cast<double>(processMemory.WorkingSetSize) / bytesPerMegabyte);
#else
        return 0.0F;
#endif
    }

    void renderUI()
    {
        // UI OVERLAY --------------------------------------------------------
        // This runs after renderScene(). Depth testing is disabled so the
        // debug window always appears on top of the 3D scene.
        glDisable(GL_DEPTH_TEST);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();

        // ENGINE DEBUG -----------------------------------------------------
        // WorkingSetSize is the physical RAM currently resident for this
        // process. This is a live budget indicator, not a forced allocation
        // cap, so the editor can warn before memory pressure becomes severe.
        ImGui::Begin("Engine Debug");
        const float currentRamMB = getCurrentRamUsageMB();
        const bool memoryBudgetExceeded = currentRamMB > 512.0F;
        if (memoryBudgetExceeded) {
            ImGui::PushStyleColor(
                ImGuiCol_Text, ImVec4(1.0F, 0.15F, 0.15F, 1.0F));
        }
        ImGui::Text("RAM Usage: %.2f MB / 512 MB", currentRamMB);
        if (memoryBudgetExceeded) {
            ImGui::PopStyleColor();
        }
        ImGui::End();

        // SCENE HIERARCHY ---------------------------------------------------
        // This panel owns creation and selection. Entities start empty; attach
        // an OBJ from the Inspector to give one visible geometry.
        ImGui::Begin("Scene Hierarchy");

        if (ImGui::Button("Spawn Default Cube")) {
            if (!defaultCubeModel_) {
                // All default cubes share one immutable VAO/VBO/EBO and one
                // one-pixel texture, minimizing both RAM and VRAM use.
                defaultCubeModel_ = createDefaultCubeModel();
            }

            const std::size_t newIndex = sceneEntities.size();
            Entity cube;
            cube.name = "Cube " + std::to_string(newIndex + 1);
            cube.position.x = static_cast<float>(newIndex % 5) * 1.5F;
            cube.position.z = -static_cast<float>(newIndex / 5) * 1.5F;
            cube.model = defaultCubeModel_;
            cube.modelPath = "Procedural Default Cube";

            sceneEntities.push_back(std::move(cube));
            createPhysicsBody(sceneEntities.back());
            selectedEntityIndex = static_cast<int>(newIndex);
            modelLoadMessage_.clear();
        }

        if (ImGui::Button("Create Empty Entity")) {
            const std::size_t newIndex = sceneEntities.size();

            Entity entity;
            entity.name = "Entity " + std::to_string(newIndex + 1);
            entity.position.x = static_cast<float>(newIndex % 5) * 1.5F;
            entity.position.z = -static_cast<float>(newIndex / 5) * 1.5F;

            sceneEntities.push_back(std::move(entity));
            createPhysicsBody(sceneEntities.back());
            selectedEntityIndex = static_cast<int>(newIndex);
        }

        for (std::size_t index = 0; index < sceneEntities.size(); ++index) {
            const bool isSelected =
                selectedEntityIndex == static_cast<int>(index);

            ImGui::PushID(static_cast<int>(index));
            if (ImGui::Selectable(sceneEntities[index].name.c_str(), isSelected)) {
                selectedEntityIndex = static_cast<int>(index);
                modelLoadMessage_.clear();
            }
            ImGui::PopID();
        }

        ImGui::End();

        // INSPECTOR ---------------------------------------------------------
        // glm::value_ptr exposes the three contiguous float components ImGui's
        // DragFloat3 expects. Changes are therefore visible in the next draw.
        ImGui::Begin("Inspector");

        const bool hasValidSelection =
            selectedEntityIndex >= 0 &&
            static_cast<std::size_t>(selectedEntityIndex) < sceneEntities.size();

        if (hasValidSelection) {
            Entity& selectedEntity =
                sceneEntities[static_cast<std::size_t>(selectedEntityIndex)];

            ImGui::TextUnformatted(selectedEntity.name.c_str());
            ImGui::Separator();
            if (ImGui::Checkbox("Active", &selectedEntity.isActive) &&
                selectedEntity.rigidBody != nullptr) {
                selectedEntity.rigidBody->setIsActive(selectedEntity.isActive);
            }

            if (ImGui::Checkbox("Dynamic", &selectedEntity.isDynamic)) {
                if (selectedEntity.rigidBody != nullptr) {
                    // Preserve the solver-owned pose exactly. Destroying the
                    // old body removes its broad-phase proxy and all cached
                    // contact manifolds before its replacement is created.
                    const reactphysics3d::Transform currentTransform =
                        selectedEntity.rigidBody->getTransform();

                    physicsWorld_->destroyRigidBody(selectedEntity.rigidBody);
                    selectedEntity.rigidBody = nullptr;
                    selectedEntity.collider = nullptr;

                    // CollisionShape lifetime is managed by PhysicsCommon,
                    // independently of the body/collider that used it.
                    if (selectedEntity.collisionShape != nullptr) {
                        physicsCommon_.destroyBoxShape(
                            selectedEntity.collisionShape);
                        selectedEntity.collisionShape = nullptr;
                    }

                    selectedEntity.collisionShape =
                        physicsCommon_.createBoxShape(
                            makeColliderHalfExtents(selectedEntity.scale));
                    selectedEntity.rigidBody =
                        physicsWorld_->createRigidBody(currentTransform);
                    selectedEntity.collider =
                        selectedEntity.rigidBody->addCollider(
                            selectedEntity.collisionShape,
                            reactphysics3d::Transform::identity());

                    // Reapply per-entity filtering because the old collider
                    // (and all of its settings) was destroyed with the body.
                    selectedEntity.collider->setCollisionCategoryBits(
                        selectedEntity.collisionCategory);
                    selectedEntity.collider->setCollideWithMaskBits(
                        selectedEntity.collisionMask);

                    if (selectedEntity.isDynamic) {
                        selectedEntity.rigidBody->setType(
                            reactphysics3d::BodyType::DYNAMIC);

                        constexpr reactphysics3d::decimal mass = 1.0F;
                        selectedEntity.rigidBody->setMass(mass);

                        // In ReactPhysics3D 0.10.2 the inertia calculation is
                        // exposed by CollisionShape rather than Collider.
                        const reactphysics3d::Vector3 inertia =
                            selectedEntity.collider->getCollisionShape()
                                ->getLocalInertiaTensor(mass);
                        selectedEntity.rigidBody->setLocalInertiaTensor(inertia);

                        selectedEntity.rigidBody->setIsActive(true);
                        selectedEntity.rigidBody->setIsSleeping(false);
                    } else {
                        selectedEntity.rigidBody->setType(
                            reactphysics3d::BodyType::STATIC);
                        selectedEntity.rigidBody->setIsActive(
                            selectedEntity.isActive);
                    }
                }
            }

            ImGui::SeparatorText("Transform Gizmo");
            if (ImGui::RadioButton(
                    "Move", gizmoOperation_ == ImGuizmo::TRANSLATE)) {
                gizmoOperation_ = ImGuizmo::TRANSLATE;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton(
                    "Rotate", gizmoOperation_ == ImGuizmo::ROTATE)) {
                gizmoOperation_ = ImGuizmo::ROTATE;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton(
                    "Scale", gizmoOperation_ == ImGuizmo::SCALE)) {
                gizmoOperation_ = ImGuizmo::SCALE;
            }

            if (gizmoOperation_ != ImGuizmo::SCALE) {
                if (ImGui::RadioButton("Local", gizmoMode_ == ImGuizmo::LOCAL)) {
                    gizmoMode_ = ImGuizmo::LOCAL;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("World", gizmoMode_ == ImGuizmo::WORLD)) {
                    gizmoMode_ = ImGuizmo::WORLD;
                }
            }

            bool transformChanged = ImGui::DragFloat3(
                "Position", glm::value_ptr(selectedEntity.position), 0.05F);
            transformChanged |= ImGui::DragFloat3(
                "Rotation", glm::value_ptr(selectedEntity.rotation), 0.5F);
            if (transformChanged && selectedEntity.rigidBody != nullptr) {
                // Inspector edits teleport the body once. From the next physics
                // step onward, the rigid body is authoritative again.
                selectedEntity.rigidBody->setTransform(
                    makePhysicsTransform(selectedEntity));
                selectedEntity.rigidBody->setIsSleeping(false);
            }

            if (ImGui::DragFloat3(
                    "Scale", glm::value_ptr(selectedEntity.scale), 0.05F) &&
                selectedEntity.collisionShape != nullptr) {
                selectedEntity.collisionShape->setHalfExtents(
                    makeColliderHalfExtents(selectedEntity.scale));
                if (selectedEntity.isDynamic &&
                    selectedEntity.rigidBody != nullptr) {
                    selectedEntity.rigidBody->updateMassPropertiesFromColliders();
                }
            }

            ImGui::SeparatorText("Collision Filtering");
            ImGui::TextDisabled("16-bit bitfields (0-65535).");
            int collisionCategory =
                static_cast<int>(selectedEntity.collisionCategory);
            if (ImGui::InputInt("Category", &collisionCategory)) {
                selectedEntity.collisionCategory = static_cast<std::uint16_t>(
                    std::clamp(collisionCategory, 0, 0xFFFF));
                if (selectedEntity.collider != nullptr) {
                    selectedEntity.collider->setCollisionCategoryBits(
                        selectedEntity.collisionCategory);
                }
            }

            int collisionMask = static_cast<int>(selectedEntity.collisionMask);
            if (ImGui::InputInt("Collision Mask", &collisionMask)) {
                selectedEntity.collisionMask = static_cast<std::uint16_t>(
                    std::clamp(collisionMask, 0, 0xFFFF));
                if (selectedEntity.collider != nullptr) {
                    selectedEntity.collider->setCollideWithMaskBits(
                        selectedEntity.collisionMask);
                }
            }

            ImGui::Separator();
            ImGui::TextUnformatted("Model Asset");
            if (!selectedEntity.modelPath.empty()) {
                ImGui::TextWrapped("%s", selectedEntity.modelPath.c_str());
            } else {
                ImGui::TextDisabled("No model loaded");
            }

            if (ImGui::Button("Browse for Model...")) {
                const std::vector<std::string> selectedPaths =
                    pfd::open_file(
                        "Select a 3D Model",
                        ".",
                        {"3D Models", "*.obj *.gltf",
                         "Wavefront OBJ", "*.obj",
                         "glTF", "*.gltf"})
                        .result();

                // An empty result means the user canceled the native dialog.
                if (!selectedPaths.empty()) {
                    try {
                        const std::filesystem::path path(selectedPaths.front());
                        std::shared_ptr<Model> loadedModel =
                            std::make_shared<Model>(path);
                        selectedEntity.model = std::move(loadedModel);
                        selectedEntity.modelPath = path.string();
                        modelLoadMessage_ = "Loaded: " + selectedEntity.modelPath;
                        modelLoadFailed_ = false;
                    } catch (const std::exception& exception) {
                        modelLoadMessage_ = exception.what();
                        modelLoadFailed_ = true;
                    }
                }
            }

            if (!modelLoadMessage_.empty()) {
                const ImVec4 messageColor = modelLoadFailed_
                    ? ImVec4(1.0F, 0.35F, 0.35F, 1.0F)
                    : ImVec4(0.35F, 1.0F, 0.45F, 1.0F);
                ImGui::TextColored(messageColor, "%s", modelLoadMessage_.c_str());
            }

            ImGui::Separator();
            ImGui::PushStyleColor(
                ImGuiCol_Button, ImVec4(0.65F, 0.12F, 0.12F, 1.0F));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonHovered, ImVec4(0.82F, 0.18F, 0.18F, 1.0F));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonActive, ImVec4(0.50F, 0.08F, 0.08F, 1.0F));
            const bool deleteEntity = ImGui::Button("Delete Entity");
            ImGui::PopStyleColor(3);

            // Erase only after the final use of selectedEntity above. Erasing
            // earlier would invalidate this reference and could crash the UI.
            if (deleteEntity) {
                destroyPhysicsBody(selectedEntity);
                sceneEntities.erase(
                    sceneEntities.begin() + selectedEntityIndex);
                selectedEntityIndex = -1;
                modelLoadMessage_.clear();
            }
        } else {
            selectedEntityIndex = -1;
            ImGui::TextDisabled("Select an entity to edit its transform.");
        }

        ImGui::End();

        // Draw and handle the selected entity's transform arrows after the
        // panels have been built, but before ImGui generates its draw data.
        renderTransformGizmo();

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    void renderFrame()
    {
        renderScene(); // First: render all model entities into the 3D scene.
        renderUI();    // Second: render ImGui over the finished 3D scene.
    }

    void shutdown() noexcept
    {
        if (imguiOpenGLInitialized_) {
            ImGui_ImplOpenGL3_Shutdown();
            imguiOpenGLInitialized_ = false;
        }
        if (imguiGlfwInitialized_) {
            ImGui_ImplGlfw_Shutdown();
            imguiGlfwInitialized_ = false;
        }
        if (imguiContextCreated_) {
            ImGui::DestroyContext();
            imguiContextCreated_ = false;
        }

        // Destroy bodies before their collision shapes, then destroy the world.
        // ReactPhysics3D factory-owned objects must never be deleted directly.
        for (Entity& entity : sceneEntities) {
            destroyPhysicsBody(entity);
        }

        if (physicsWorld_ != nullptr) {
            physicsCommon_.destroyPhysicsWorld(physicsWorld_);
            physicsWorld_ = nullptr;
        }

        // Models own textures and Mesh GPU buffers. Release every shared_ptr
        // while the OpenGL context is still alive so their destructors are safe.
        sceneEntities.clear();
        defaultCubeModel_.reset();
        selectedEntityIndex = -1;

        if (shaderProgram_ != 0) {
            glDeleteProgram(shaderProgram_);
            shaderProgram_ = 0;
        }

        if (window_ != nullptr) {
            glfwDestroyWindow(window_);
            window_ = nullptr;
        }
        if (glfwInitialized_) {
            glfwTerminate();
            glfwInitialized_ = false;
        }
    }

    GLFWwindow* window_ = nullptr;

    GLuint shaderProgram_ = 0;
    GLint modelLocation_ = -1;
    GLint viewLocation_ = -1;
    GLint projectionLocation_ = -1;
    GLint textureLocation_ = -1;

    glm::mat4 projection_{1.0F};

    CameraState camera_{};

    std::shared_ptr<Model> defaultCubeModel_;
    ImGuizmo::OPERATION gizmoOperation_ = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE gizmoMode_ = ImGuizmo::LOCAL;

    reactphysics3d::PhysicsCommon physicsCommon_;
    reactphysics3d::PhysicsWorld* physicsWorld_ = nullptr;

    float deltaTime_ = 0.0F;
    float lastFrame_ = 0.0F;
    float lastMouseX_ = 0.0F;
    float lastMouseY_ = 0.0F;

    std::string modelLoadMessage_;

    bool glfwInitialized_ = false;
    bool imguiContextCreated_ = false;
    bool imguiGlfwInitialized_ = false;
    bool imguiOpenGLInitialized_ = false;
    bool mouseLookActive_ = false;
    bool firstMouseSample_ = true;
    bool modelLoadFailed_ = false;
};

} // namespace

int main()
{
    try {
        Application application;
        application.run();
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "Fatal error: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
