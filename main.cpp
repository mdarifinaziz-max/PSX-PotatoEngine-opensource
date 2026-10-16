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
#include <imgui_internal.h>
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
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// This is our first tiny "component": every scene object has a display name
// and a transform. Rotation values are stored as degrees because that is much
// friendlier to edit in the Inspector than radians.
class Model;

enum class TargetType { SELF, PARENT, CHILDREN, TAGGED };
enum class Operator { EQUALS, NOT_EQUALS, GREATER, LESS };

enum class ActionType {
    APPLY_FORCE,
    SET_VELOCITY,
    TELEPORT,
    SPAWN_ENTITY,
    DESTROY_ENTITY,
    SET_VAR,
    BROADCAST_SIGNAL,
    PARENT_TO,
    UNPARENT,
    SHIFT_ORIGIN,
    MOVE_GRID_ITEM,
    GENERATE_CHUNK
};

enum class EngineState {
    EDIT,
    PLAY
};

enum class LogicCardType {
    NONE,
    TRIGGER,
    CONDITION,
    ACTION
};

struct Condition {
    std::string varName = "Health";
    Operator op = Operator::EQUALS;
    float targetValue = 0.0F;
};

struct Action {
    ActionType type = ActionType::APPLY_FORCE;
    TargetType target = TargetType::SELF;
    std::string targetTag = "Default";
    glm::vec3 vecData{0.0F, 1.0F, 0.0F};
    std::string strData;
    float floatData = 1.0F;
};

struct LogicRule {
    std::string eventTriggerName = "On Play Start";
    int targetKey = GLFW_KEY_SPACE;
    std::string targetStr;
    float timerInterval = 1.0F;
    float timerAccumulator = 0.0F;
    std::vector<Condition> conditions;
    std::vector<Action> actions;

    // Runtime-only edge state kept inline to avoid a separate hash lookup for
    // every input rule on every frame.
    bool keyWasDown = false;
};

struct Entity {
    std::string uuid;
    std::string parentUuid;
    std::string tag = "Default";
    std::unordered_map<std::string, float> localVars;
    std::string currentState = "Idle";

    std::string name;
    glm::vec3 position{0.0F, 0.0F, 0.0F};
    glm::vec3 localOffset{0.0F, 0.0F, 0.0F};
    glm::vec3 rotation{0.0F, 0.0F, 0.0F};
    glm::vec3 scale{1.0F, 1.0F, 1.0F};
    bool isActive = true;
    bool isVisible = true;
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
    std::vector<LogicRule> logicLifecycle;
    std::vector<LogicRule> logicInput;
    std::vector<LogicRule> logicPhysics;
    std::vector<LogicRule> logicBrain;
    std::vector<LogicRule> logicWorld;

    // Set by the ReactPhysics3D listener during update(), then consumed by
    // the next frame's pre-physics logic pass.
    bool collisionEnteredThisFrame = false;
    std::string touchedByNameThisFrame;
};

// For now, a contiguous vector is the entire scene/world database. This is
// cache-friendly, simple to inspect, and more than adequate for a small level.
std::vector<Entity> sceneEntities;
std::vector<Entity> sceneBackup;
int selectedEntityIndex = -1;
EngineState currentState = EngineState::EDIT;

class SceneCollisionListener final : public reactphysics3d::EventListener {
public:
    void onContact(
        const reactphysics3d::CollisionCallback::CallbackData& callbackData)
        override
    {
        for (reactphysics3d::uint32 pairIndex = 0;
             pairIndex < callbackData.getNbContactPairs();
             ++pairIndex) {
            const auto contactPair = callbackData.getContactPair(pairIndex);
            if (contactPair.getEventType() !=
                reactphysics3d::CollisionCallback::ContactPair::EventType::ContactStart) {
                continue;
            }

            const reactphysics3d::Body* body1 = contactPair.getBody1();
            const reactphysics3d::Body* body2 = contactPair.getBody2();
            Entity* entity1 = nullptr;
            Entity* entity2 = nullptr;
            for (Entity& entity : sceneEntities) {
                if (entity.rigidBody == body1) {
                    entity1 = &entity;
                }
                if (entity.rigidBody == body2) {
                    entity2 = &entity;
                }
            }

            if (entity1 != nullptr) {
                entity1->collisionEnteredThisFrame = true;
                entity1->touchedByNameThisFrame =
                    entity2 != nullptr ? entity2->name : "Anything";
            }
            if (entity2 != nullptr) {
                entity2->collisionEnteredThisFrame = true;
                entity2->touchedByNameThisFrame =
                    entity1 != nullptr ? entity1->name : "Anything";
            }
        }
    }
};

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
            if (currentState == EngineState::PLAY) {
                runPlayModeFrame();
            } else {
                // Physics is paused in Edit Mode, but hierarchy relationships
                // must still update so parent movement is immediately visible.
                syncParentTransforms();
            }

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

        physicsWorld_->setEventListener(&collisionListener_);
    }

    void initializeUI()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        imguiContextCreated_ = true;
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
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
        configurePhysicsBody(entity);
    }

    void configurePhysicsBody(Entity& entity)
    {
        entity.collider->setCollisionCategoryBits(entity.collisionCategory);
        entity.collider->setCollideWithMaskBits(entity.collisionMask);
        entity.rigidBody->setType(
            entity.isDynamic ? reactphysics3d::BodyType::DYNAMIC
                             : reactphysics3d::BodyType::STATIC);

        if (entity.isDynamic) {
            constexpr reactphysics3d::decimal mass = 1.0F;
            entity.rigidBody->setMass(mass);
            entity.rigidBody->setLocalInertiaTensor(
                entity.collisionShape->getLocalInertiaTensor(mass));
        }

        const bool physicsActive =
            entity.isActive && entity.parentUuid.empty();
        entity.rigidBody->setIsActive(physicsActive);
        if (physicsActive && entity.isDynamic) {
            entity.rigidBody->setIsSleeping(false);
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

    void rebuildPhysicsBody(
        Entity& entity,
        const reactphysics3d::Transform* replacementTransform = nullptr)
    {
        if (physicsWorld_ == nullptr || entity.rigidBody == nullptr) {
            return;
        }

        // Recreating the body clears all broad-phase and contact-cache state.
        const reactphysics3d::Transform currentTransform =
            replacementTransform != nullptr
                ? *replacementTransform
                : entity.rigidBody->getTransform();
        destroyPhysicsBody(entity);

        entity.collisionShape =
            physicsCommon_.createBoxShape(makeColliderHalfExtents(entity.scale));
        entity.rigidBody = physicsWorld_->createRigidBody(currentTransform);
        entity.collider = entity.rigidBody->addCollider(
            entity.collisionShape, reactphysics3d::Transform::identity());
        configurePhysicsBody(entity);
    }

    static Entity makeEntitySnapshot(const Entity& source)
    {
        // Copy editor/gameplay data, but never copy PhysicsWorld-owned raw
        // pointers. Model assets are immutable and intentionally shared to
        // keep Play snapshots cheap on low-RAM machines.
        Entity snapshot;
        snapshot.uuid = source.uuid;
        snapshot.parentUuid = source.parentUuid;
        snapshot.tag = source.tag;
        snapshot.localVars = source.localVars;
        snapshot.currentState = source.currentState;
        snapshot.name = source.name;
        snapshot.position = source.position;
        snapshot.localOffset = source.localOffset;
        snapshot.rotation = source.rotation;
        snapshot.scale = source.scale;
        snapshot.isActive = source.isActive;
        snapshot.isVisible = source.isVisible;
        snapshot.isDynamic = source.isDynamic;
        snapshot.collisionCategory = source.collisionCategory;
        snapshot.collisionMask = source.collisionMask;
        snapshot.model = source.model;
        snapshot.modelPath = source.modelPath;
        snapshot.logicLifecycle = source.logicLifecycle;
        snapshot.logicInput = source.logicInput;
        snapshot.logicPhysics = source.logicPhysics;
        snapshot.logicBrain = source.logicBrain;
        snapshot.logicWorld = source.logicWorld;

        auto resetRuntimeState = [](std::vector<LogicRule>& rules) {
            for (LogicRule& rule : rules) {
                rule.keyWasDown = false;
                rule.timerAccumulator = 0.0F;
            }
        };
        resetRuntimeState(snapshot.logicLifecycle);
        resetRuntimeState(snapshot.logicInput);
        resetRuntimeState(snapshot.logicPhysics);
        resetRuntimeState(snapshot.logicBrain);
        resetRuntimeState(snapshot.logicWorld);
        snapshot.collisionEnteredThisFrame = false;
        snapshot.touchedByNameThisFrame.clear();
        return snapshot;
    }

    void captureSceneBackup()
    {
        sceneBackup.clear();
        sceneBackup.reserve(sceneEntities.size());
        for (const Entity& entity : sceneEntities) {
            sceneBackup.push_back(makeEntitySnapshot(entity));
        }
    }

    void startPlayMode()
    {
        captureSceneBackup();
        signalQueue_.clear();
        pendingSpawns_.clear();
        parentSyncState_.clear();
        lifecyclePending_ = true;

        // Reset one-frame input/contact state at the Edit -> Play boundary.
        for (Entity& entity : sceneEntities) {
            entity.collisionEnteredThisFrame = false;
            entity.touchedByNameThisFrame.clear();
            auto resetRuntimeState = [](std::vector<LogicRule>& rules) {
                for (LogicRule& rule : rules) {
                    rule.keyWasDown = false;
                    rule.timerAccumulator = 0.0F;
                }
            };
            resetRuntimeState(entity.logicLifecycle);
            resetRuntimeState(entity.logicInput);
            resetRuntimeState(entity.logicPhysics);
            resetRuntimeState(entity.logicBrain);
            resetRuntimeState(entity.logicWorld);
            if (entity.rigidBody != nullptr) {
                const bool physicsActive =
                    entity.isActive && entity.parentUuid.empty();
                entity.rigidBody->setIsActive(physicsActive);
                if (physicsActive && entity.isDynamic) {
                    entity.rigidBody->setIsSleeping(false);
                }
            }
        }

        currentState = EngineState::PLAY;
    }

    void stopPlayMode()
    {
        // Remove every live body first so ReactPhysics3D can discard its
        // broad-phase proxies and contact caches before entity storage moves.
        for (Entity& entity : sceneEntities) {
            destroyPhysicsBody(entity);
        }
        sceneEntities.clear();

        sceneEntities.reserve(sceneBackup.size());
        for (const Entity& snapshot : sceneBackup) {
            sceneEntities.push_back(makeEntitySnapshot(snapshot));
        }

        // Restored entities receive entirely new bodies/colliders at their
        // original editor transforms and with their saved physics settings.
        for (Entity& entity : sceneEntities) {
            createPhysicsBody(entity);
        }

        sceneBackup.clear();
        signalQueue_.clear();
        pendingSpawns_.clear();
        lifecyclePending_ = false;
        if (selectedEntityIndex < 0 ||
            static_cast<std::size_t>(selectedEntityIndex) >= sceneEntities.size()) {
            selectedEntityIndex = -1;
        }
        currentState = EngineState::EDIT;
    }

    std::string createEntityUuid()
    {
        return "entity-" + std::to_string(nextEntityUuid_++);
    }

    static bool conditionsPass(const Entity& entity, const LogicRule& rule)
    {
        for (const Condition& condition : rule.conditions) {
            const auto variable = entity.localVars.find(condition.varName);
            const float value = variable != entity.localVars.end()
                ? variable->second
                : 0.0F;

            bool passed = false;
            switch (condition.op) {
            case Operator::EQUALS:
                passed = std::abs(value - condition.targetValue) < 0.0001F;
                break;
            case Operator::NOT_EQUALS:
                passed = std::abs(value - condition.targetValue) >= 0.0001F;
                break;
            case Operator::GREATER:
                passed = value > condition.targetValue;
                break;
            case Operator::LESS:
                passed = value < condition.targetValue;
                break;
            }
            if (!passed) {
                return false;
            }
        }
        return true;
    }

    std::vector<std::size_t> resolveTargets(
        std::size_t sourceIndex, const Action& action) const
    {
        std::vector<std::size_t> targets;
        if (sourceIndex >= sceneEntities.size()) {
            return targets;
        }

        const Entity& source = sceneEntities[sourceIndex];
        switch (action.target) {
        case TargetType::SELF:
            targets.push_back(sourceIndex);
            break;
        case TargetType::PARENT:
            for (std::size_t index = 0; index < sceneEntities.size(); ++index) {
                if (sceneEntities[index].uuid == source.parentUuid) {
                    targets.push_back(index);
                    break;
                }
            }
            break;
        case TargetType::CHILDREN:
            for (std::size_t index = 0; index < sceneEntities.size(); ++index) {
                if (sceneEntities[index].parentUuid == source.uuid) {
                    targets.push_back(index);
                }
            }
            break;
        case TargetType::TAGGED:
            for (std::size_t index = 0; index < sceneEntities.size(); ++index) {
                if (sceneEntities[index].tag == action.targetTag) {
                    targets.push_back(index);
                }
            }
            break;
        }
        return targets;
    }

    void teleportEntity(Entity& entity, const glm::vec3& position)
    {
        entity.position = position;
        if (entity.rigidBody == nullptr) {
            return;
        }
        entity.rigidBody->setTransform(makePhysicsTransform(entity));
        entity.rigidBody->setLinearVelocity(
            reactphysics3d::Vector3(0.0F, 0.0F, 0.0F));
        entity.rigidBody->setAngularVelocity(
            reactphysics3d::Vector3(0.0F, 0.0F, 0.0F));
        entity.rigidBody->setIsSleeping(false);
    }

    void executeAction(
        std::size_t sourceIndex, std::size_t targetIndex, const Action& action)
    {
        if (sourceIndex >= sceneEntities.size() ||
            targetIndex >= sceneEntities.size()) {
            return;
        }

        Entity& target = sceneEntities[targetIndex];
        switch (action.type) {
        case ActionType::APPLY_FORCE:
            if (target.isActive && target.isDynamic &&
                target.rigidBody != nullptr) {
                const reactphysics3d::Vector3 force(
                    action.vecData.x * action.floatData,
                    action.vecData.y * action.floatData,
                    action.vecData.z * action.floatData);
                target.rigidBody->applyWorldForceAtCenterOfMass(force);
                target.rigidBody->setIsSleeping(false);
            }
            break;
        case ActionType::SET_VELOCITY:
            if (target.isActive && target.isDynamic &&
                target.rigidBody != nullptr) {
                target.rigidBody->setLinearVelocity(
                    reactphysics3d::Vector3(
                        action.vecData.x, action.vecData.y, action.vecData.z));
                target.rigidBody->setIsSleeping(false);
            }
            break;
        case ActionType::TELEPORT:
        case ActionType::MOVE_GRID_ITEM:
            teleportEntity(target, action.vecData);
            break;
        case ActionType::SPAWN_ENTITY: {
            Entity spawned;
            spawned.uuid = createEntityUuid();
            spawned.name = action.strData.empty()
                ? "Spawned Entity " + std::to_string(nextEntityUuid_ - 1)
                : action.strData;
            spawned.tag = target.tag;
            spawned.position = target.position + action.vecData;
            spawned.model = target.model ? target.model : defaultCubeModel_;
            spawned.modelPath = target.modelPath;
            pendingSpawns_.push_back(std::move(spawned));
            break;
        }
        case ActionType::DESTROY_ENTITY:
            destroyPhysicsBody(target);
            target.isVisible = false;
            target.isActive = false;
            break;
        case ActionType::SET_VAR:
            if (!action.strData.empty()) {
                target.localVars[action.strData] = action.floatData;
            }
            break;
        case ActionType::BROADCAST_SIGNAL:
            if (!action.strData.empty()) {
                signalQueue_.push_back(action.strData);
            }
            break;
        case ActionType::PARENT_TO: {
            // Parent fields store UUIDs, but the author-facing action accepts
            // a friendly tag or entity name. Prefer tags, then fall back to a
            // name match, and never allow an entity to parent itself.
            Entity* targetParent = nullptr;
            for (Entity& candidate : sceneEntities) {
                if (candidate.uuid != target.uuid &&
                    candidate.tag == action.strData) {
                    targetParent = &candidate;
                    break;
                }
            }
            if (targetParent == nullptr) {
                for (Entity& candidate : sceneEntities) {
                    if (candidate.uuid != target.uuid &&
                        candidate.name == action.strData) {
                        targetParent = &candidate;
                        break;
                    }
                }
            }

            if (targetParent != nullptr) {
                // Preserve the current world-space placement when attaching.
                target.localOffset = target.position - targetParent->position;
                target.parentUuid = targetParent->uuid;

                // A parent-controlled child must not simultaneously be moved
                // by the physics solver.
                if (target.rigidBody != nullptr) {
                    target.rigidBody->setLinearVelocity(
                        reactphysics3d::Vector3(0.0F, 0.0F, 0.0F));
                    target.rigidBody->setAngularVelocity(
                        reactphysics3d::Vector3(0.0F, 0.0F, 0.0F));
                    target.rigidBody->setIsActive(false);
                }
            }
            break;
        }
        case ActionType::UNPARENT:
            target.parentUuid.clear();
            target.localOffset = glm::vec3(0.0F);
            if (target.rigidBody != nullptr) {
                // The dormant body may still be at its pre-parenting pose.
                // Move it to the current visual transform before waking it.
                target.rigidBody->setTransform(makePhysicsTransform(target));
                target.rigidBody->setLinearVelocity(
                    reactphysics3d::Vector3(0.0F, 0.0F, 0.0F));
                target.rigidBody->setAngularVelocity(
                    reactphysics3d::Vector3(0.0F, 0.0F, 0.0F));
                target.rigidBody->setIsActive(true);
                if (target.isDynamic) {
                    target.rigidBody->setIsSleeping(false);
                }
            }
            break;
        case ActionType::SHIFT_ORIGIN:
            teleportEntity(target, target.position - action.vecData);
            break;
        case ActionType::GENERATE_CHUNK:
            // Lightweight scaffolding: a future chunk streamer can consume
            // this signal without changing the authoring data format.
            signalQueue_.push_back("GenerateChunk:" + action.strData);
            break;
        }
    }

    void executeRule(std::size_t sourceIndex, LogicRule& rule)
    {
        if (sourceIndex >= sceneEntities.size() ||
            !conditionsPass(sceneEntities[sourceIndex], rule)) {
            return;
        }

        for (const Action& action : rule.actions) {
            // Signals belong to the world bus, not to an entity target. Send
            // them exactly once even if a Parent/Children/Tagged lookup has no
            // matches (or has many matches).
            if (action.type == ActionType::BROADCAST_SIGNAL) {
                if (!action.strData.empty()) {
                    signalQueue_.push_back(action.strData);
                }
                continue;
            }
            const std::vector<std::size_t> targets =
                resolveTargets(sourceIndex, action);
            for (const std::size_t targetIndex : targets) {
                executeAction(sourceIndex, targetIndex, action);
            }
        }
    }

    void evaluateLifecyclePhase()
    {
        if (!lifecyclePending_) {
            return;
        }
        lifecyclePending_ = false;
        for (std::size_t index = 0; index < sceneEntities.size(); ++index) {
            if (!sceneEntities[index].isActive) {
                continue;
            }
            for (LogicRule& rule : sceneEntities[index].logicLifecycle) {
                if (!sceneEntities[index].isActive) {
                    break;
                }
                executeRule(index, rule);
            }
        }
    }

    void evaluateInputPhase()
    {
        for (std::size_t index = 0; index < sceneEntities.size(); ++index) {
            Entity& entity = sceneEntities[index];
            if (!entity.isActive) {
                continue;
            }
            for (LogicRule& rule : entity.logicInput) {
                if (!entity.isActive) {
                    break;
                }
                const bool validKey = rule.targetKey >= GLFW_KEY_SPACE &&
                    rule.targetKey <= GLFW_KEY_LAST;
                const bool keyDown = validKey &&
                    glfwGetKey(window_, rule.targetKey) == GLFW_PRESS;
                bool triggered = false;
                if (rule.eventTriggerName == "Key Pressed") {
                    triggered = keyDown && !rule.keyWasDown;
                } else if (rule.eventTriggerName == "Key Held") {
                    triggered = keyDown;
                } else if (rule.eventTriggerName == "Key Released") {
                    triggered = !keyDown && rule.keyWasDown;
                } else if (rule.eventTriggerName == "Mouse Pressed") {
                    const bool validButton = rule.targetKey >= 0 &&
                        rule.targetKey <= GLFW_MOUSE_BUTTON_LAST;
                    const bool buttonDown = validButton &&
                        glfwGetMouseButton(window_, rule.targetKey) == GLFW_PRESS;
                    triggered = buttonDown && !rule.keyWasDown;
                    rule.keyWasDown = buttonDown;
                    if (triggered) {
                        executeRule(index, rule);
                    }
                    continue;
                }
                rule.keyWasDown = keyDown;
                if (triggered) {
                    executeRule(index, rule);
                }
            }
        }
    }

    void evaluateWorldPhase()
    {
        // Swap first: signals emitted by world receivers wait until the next
        // frame, preventing recursive signal storms.
        std::vector<std::string> signals;
        signals.swap(signalQueue_);
        for (std::size_t index = 0; index < sceneEntities.size(); ++index) {
            Entity& entity = sceneEntities[index];
            if (!entity.isActive) {
                continue;
            }
            for (LogicRule& rule : entity.logicWorld) {
                if (!entity.isActive) {
                    break;
                }
                const bool received = rule.eventTriggerName == "Signal Received" &&
                    std::find(signals.begin(), signals.end(), rule.targetStr) !=
                        signals.end();
                if (received) {
                    executeRule(index, rule);
                }
            }
        }
    }

    void evaluateBrainPhase()
    {
        for (std::size_t index = 0; index < sceneEntities.size(); ++index) {
            Entity& entity = sceneEntities[index];
            if (!entity.isActive) {
                continue;
            }
            for (LogicRule& rule : entity.logicBrain) {
                if (!entity.isActive) {
                    break;
                }
                bool triggered = rule.eventTriggerName == "Every Frame";
                if (rule.eventTriggerName == "Timer") {
                    rule.timerAccumulator += deltaTime_;
                    const float interval = std::max(rule.timerInterval, 0.001F);
                    if (rule.timerAccumulator >= interval) {
                        rule.timerAccumulator = std::fmod(
                            rule.timerAccumulator, interval);
                        triggered = true;
                    }
                } else if (rule.eventTriggerName == "State Is") {
                    triggered = entity.currentState == rule.targetStr;
                }
                if (triggered) {
                    executeRule(index, rule);
                }
            }
        }
    }

    void evaluatePhysicsPhase()
    {
        for (std::size_t index = 0; index < sceneEntities.size(); ++index) {
            Entity& entity = sceneEntities[index];
            if (entity.isActive) {
                for (LogicRule& rule : entity.logicPhysics) {
                    if (!entity.isActive) {
                        break;
                    }
                    const bool acceptsAnything = rule.targetStr.empty() ||
                        rule.targetStr == "Anything";
                    const bool collisionTriggered =
                        rule.eventTriggerName == "Collision Enter" &&
                        entity.collisionEnteredThisFrame &&
                        (acceptsAnything ||
                         rule.targetStr == entity.touchedByNameThisFrame);
                    if (collisionTriggered) {
                        executeRule(index, rule);
                    }
                }
            }
            entity.collisionEnteredThisFrame = false;
            entity.touchedByNameThisFrame.clear();
        }
    }

    static void updateLocalOffsetFromWorld(Entity& child)
    {
        if (child.parentUuid.empty()) {
            return;
        }
        for (const Entity& candidate : sceneEntities) {
            if (candidate.uuid == child.parentUuid &&
                candidate.uuid != child.uuid) {
                child.localOffset = child.position - candidate.position;
                return;
            }
        }
    }

    void syncParentTransform(std::size_t childIndex)
    {
        if (childIndex >= sceneEntities.size() ||
            parentSyncState_[childIndex] == 2U) {
            return;
        }
        if (parentSyncState_[childIndex] == 1U) {
            // Break cyclic hierarchy data instead of recursing forever.
            sceneEntities[childIndex].parentUuid.clear();
            sceneEntities[childIndex].localOffset = glm::vec3(0.0F);
            parentSyncState_[childIndex] = 2U;
            return;
        }

        Entity& child = sceneEntities[childIndex];
        if (child.parentUuid.empty()) {
            parentSyncState_[childIndex] = 2U;
            return;
        }

        parentSyncState_[childIndex] = 1U;
        std::size_t parentIndex = sceneEntities.size();
        for (std::size_t index = 0; index < sceneEntities.size(); ++index) {
            if (index != childIndex &&
                sceneEntities[index].uuid == child.parentUuid) {
                parentIndex = index;
                break;
            }
        }

        if (parentIndex < sceneEntities.size()) {
            syncParentTransform(parentIndex);
            // Recursion may have broken a cyclic link involving this child.
            if (!child.parentUuid.empty()) {
                child.position =
                    sceneEntities[parentIndex].position + child.localOffset;
            }
        }
        parentSyncState_[childIndex] = 2U;
    }

    void syncParentTransforms()
    {
        parentSyncState_.assign(sceneEntities.size(), 0U);
        for (std::size_t index = 0; index < sceneEntities.size(); ++index) {
            syncParentTransform(index);
        }
    }

    void flushPendingSpawns()
    {
        if (pendingSpawns_.empty()) {
            return;
        }
        sceneEntities.reserve(sceneEntities.size() + pendingSpawns_.size());
        for (Entity& entity : pendingSpawns_) {
            sceneEntities.push_back(std::move(entity));
            createPhysicsBody(sceneEntities.back());
        }
        pendingSpawns_.clear();
    }

    void runPlayModeFrame()
    {
        evaluateLifecyclePhase();
        evaluateInputPhase();
        evaluateWorldPhase();
        evaluateBrainPhase();
        updatePhysics();
        evaluatePhysicsPhase();
        syncParentTransforms();
        flushPendingSpawns();
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
                destroyPhysicsBody(entity);
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

        // Keep rendering and mouse unprojection on the same aspect ratio when
        // the editor window is resized.
        if (framebufferHeight > 0) {
            projection_ = glm::perspective(
                glm::radians(45.0F),
                static_cast<float>(framebufferWidth) /
                    static_cast<float>(framebufferHeight),
                0.1F,
                100.0F);
        }

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
            if (!entity.isActive || !entity.isVisible) {
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

    static bool intersectUnitBox(
        const glm::vec3& rayOrigin,
        const glm::vec3& rayDirection,
        const glm::mat4& model,
        float& hitDistance)
    {
        // Transforming the ray into entity-local space turns the rotated and
        // scaled collider back into a cheap axis-aligned unit cube.
        if (std::abs(glm::determinant(model)) < 0.000001F) {
            return false;
        }
        const glm::mat4 inverseModel = glm::inverse(model);
        const glm::vec3 localOrigin = glm::vec3(
            inverseModel * glm::vec4(rayOrigin, 1.0F));
        const glm::vec3 localDirection = glm::vec3(
            inverseModel * glm::vec4(rayDirection, 0.0F));

        constexpr float boxMinimum = -0.5F;
        constexpr float boxMaximum = 0.5F;
        constexpr float epsilon = 0.000001F;
        float minimumDistance = 0.0F;
        float maximumDistance = std::numeric_limits<float>::max();

        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(localDirection[axis]) < epsilon) {
                if (localOrigin[axis] < boxMinimum ||
                    localOrigin[axis] > boxMaximum) {
                    return false;
                }
                continue;
            }

            float nearDistance =
                (boxMinimum - localOrigin[axis]) / localDirection[axis];
            float farDistance =
                (boxMaximum - localOrigin[axis]) / localDirection[axis];
            if (nearDistance > farDistance) {
                std::swap(nearDistance, farDistance);
            }
            minimumDistance = std::max(minimumDistance, nearDistance);
            maximumDistance = std::min(maximumDistance, farDistance);
            if (minimumDistance > maximumDistance) {
                return false;
            }
        }

        hitDistance = minimumDistance;
        return maximumDistance >= 0.0F;
    }

    void handleViewportPicking()
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
            io.WantCaptureMouse || ImGuizmo::IsOver() ||
            ImGuizmo::IsUsing()) {
            return;
        }

        // The current renderer draws the 3D scene directly into the complete
        // GLFW framebuffer. ImGui windows overlay it, and WantCaptureMouse
        // above prevents those windows from leaking clicks into the scene.
        int viewportWidth = 0;
        int viewportHeight = 0;
        glfwGetWindowSize(window_, &viewportWidth, &viewportHeight);
        if (viewportWidth <= 0 || viewportHeight <= 0) {
            return;
        }

        double cursorX = 0.0;
        double cursorY = 0.0;
        glfwGetCursorPos(window_, &cursorX, &cursorY);
        const float mouseXInViewport = static_cast<float>(cursorX);
        const float mouseYInViewport = static_cast<float>(cursorY);
        if (mouseXInViewport < 0.0F || mouseYInViewport < 0.0F ||
            mouseXInViewport >= static_cast<float>(viewportWidth) ||
            mouseYInViewport >= static_cast<float>(viewportHeight)) {
            return;
        }

        const float ndcX =
            (2.0F * mouseXInViewport) / static_cast<float>(viewportWidth) - 1.0F;
        const float ndcY =
            1.0F - (2.0F * mouseYInViewport) /
                static_cast<float>(viewportHeight);

        const glm::mat4 view = glm::lookAt(
            camera_.cameraPos,
            camera_.cameraPos + camera_.cameraFront,
            camera_.cameraUp);
        const glm::mat4 inverseProjectionView =
            glm::inverse(projection_ * view);

        glm::vec4 rayStartWorld = inverseProjectionView *
            glm::vec4(ndcX, ndcY, -1.0F, 1.0F);
        glm::vec4 rayEndWorld = inverseProjectionView *
            glm::vec4(ndcX, ndcY, 1.0F, 1.0F);
        if (std::abs(rayStartWorld.w) < 0.000001F ||
            std::abs(rayEndWorld.w) < 0.000001F) {
            return;
        }
        rayStartWorld /= rayStartWorld.w;
        rayEndWorld /= rayEndWorld.w;

        const glm::vec3 rayOrigin = camera_.cameraPos;
        const glm::vec3 rayDirection = glm::normalize(
            glm::vec3(rayEndWorld - rayStartWorld));

        int closestEntity = -1;
        float closestDistance = std::numeric_limits<float>::max();
        for (std::size_t index = 0; index < sceneEntities.size(); ++index) {
            const Entity& entity = sceneEntities[index];
            if (!entity.isActive || !entity.isVisible || !entity.model) {
                continue;
            }

            float hitDistance = 0.0F;
            if (intersectUnitBox(
                    rayOrigin, rayDirection, makeModelMatrix(entity),
                    hitDistance) &&
                hitDistance < closestDistance) {
                closestDistance = hitDistance;
                closestEntity = static_cast<int>(index);
            }
        }

        // Clicking empty viewport space clears the selection, matching common
        // editor behavior. A hit also highlights the same item in Hierarchy.
        selectedEntityIndex = closestEntity;
        modelLoadMessage_.clear();
    }

    void renderTransformGizmo()
    {
        if (currentState != EngineState::EDIT) {
            return;
        }

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

        const glm::vec3 previousScale = entity.scale;
        entity.position = glm::make_vec3(translation);
        updateLocalOffsetFromWorld(entity);
        entity.rotation = glm::make_vec3(rotation);
        entity.scale = glm::make_vec3(scale);

        const bool scaleChanged =
            glm::length(entity.scale - previousScale) > 0.0001F;
        if (entity.rigidBody != nullptr) {
            const reactphysics3d::Transform editedTransform =
                makePhysicsTransform(entity);

            if (scaleChanged && entity.collider != nullptr) {
                // Rebuild instead of mutating BoxShape in place. This forces
                // ReactPhysics3D to refresh the broad-phase AABB and discard
                // contact manifolds based on the old half-extents.
                rebuildPhysicsBody(entity, &editedTransform);
            } else {
                entity.rigidBody->setTransform(editedTransform);
                entity.rigidBody->setIsSleeping(false);
            }

            if (!scaleChanged && entity.isDynamic) {
                // Editor manipulation is a teleport, not a physical impulse.
                entity.rigidBody->setLinearVelocity(
                    reactphysics3d::Vector3(0.0F, 0.0F, 0.0F));
                entity.rigidBody->setAngularVelocity(
                    reactphysics3d::Vector3(0.0F, 0.0F, 0.0F));
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

    void buildDefaultDockLayout(ImGuiID dockspaceId, const ImVec2& size)
    {
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(
            dockspaceId,
            ImGuiDockNodeFlags_DockSpace |
                ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::DockBuilderSetNodeSize(dockspaceId, size);

        ImGuiID centerId = dockspaceId;
        ImGuiID leftId = ImGui::DockBuilderSplitNode(
            centerId, ImGuiDir_Left, 0.20F, nullptr, &centerId);
        ImGuiID rightId = ImGui::DockBuilderSplitNode(
            centerId, ImGuiDir_Right, 0.26F, nullptr, &centerId);
        ImGuiID bottomId = ImGui::DockBuilderSplitNode(
            centerId, ImGuiDir_Down, 0.34F, nullptr, &centerId);
        ImGuiID profilerId = ImGui::DockBuilderSplitNode(
            leftId, ImGuiDir_Down, 0.25F, nullptr, &leftId);

        ImGui::DockBuilderDockWindow("Scene Hierarchy", leftId);
        ImGui::DockBuilderDockWindow("Memory Profiler", profilerId);
        ImGui::DockBuilderDockWindow("Inspector", rightId);
        ImGui::DockBuilderDockWindow("Logic Editor", bottomId);
        ImGui::DockBuilderFinish(dockspaceId);
    }

    void renderDockSpaceHost()
    {
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        constexpr ImGuiWindowFlags hostWindowFlags =
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_MenuBar;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        ImGui::Begin("DockSpaceWindow", nullptr, hostWindowFlags);
        ImGui::PopStyleVar(3);

        const ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
        if (resetDockLayout_ || ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
            buildDefaultDockLayout(dockspaceId, viewport->WorkSize);
            resetDockLayout_ = false;
        }
        ImGui::DockSpace(
            dockspaceId,
            ImVec2(0.0F, 0.0F),
            ImGuiDockNodeFlags_PassthruCentralNode);

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("Window")) {
                ImGui::MenuItem("Scene Hierarchy", nullptr, &showHierarchy_);
                ImGui::MenuItem("Inspector", nullptr, &showInspector_);
                ImGui::MenuItem("Logic Editor", nullptr, &showLogicEditor_);
                ImGui::MenuItem("Memory Profiler", nullptr, &showProfiler_);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Layouts")) {
                if (ImGui::MenuItem("Reset to Default")) {
                    resetDockLayout_ = true;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        ImGui::End();
    }

    void renderPlayToolbar()
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(
            ImVec2(
                viewport->WorkPos.x + viewport->WorkSize.x * 0.5F,
                viewport->WorkPos.y + ImGui::GetFrameHeight() + 8.0F),
            ImGuiCond_Always,
            ImVec2(0.5F, 0.0F));

        constexpr ImGuiWindowFlags toolbarFlags =
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing;

        ImGui::SetNextWindowBgAlpha(0.90F);
        ImGui::Begin("##PlayModeToolbar", nullptr, toolbarFlags);

        if (currentState == EngineState::EDIT) {
            ImGui::PushStyleColor(
                ImGuiCol_Button, ImVec4(0.12F, 0.60F, 0.22F, 1.0F));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonHovered, ImVec4(0.18F, 0.78F, 0.30F, 1.0F));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonActive, ImVec4(0.08F, 0.45F, 0.16F, 1.0F));
            const bool playClicked = ImGui::Button("PLAY", ImVec2(90.0F, 28.0F));
            ImGui::PopStyleColor(3);
            if (playClicked) {
                startPlayMode();
            }
        } else {
            ImGui::PushStyleColor(
                ImGuiCol_Button, ImVec4(0.68F, 0.12F, 0.12F, 1.0F));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonHovered, ImVec4(0.86F, 0.20F, 0.20F, 1.0F));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonActive, ImVec4(0.52F, 0.08F, 0.08F, 1.0F));
            const bool stopClicked = ImGui::Button("STOP", ImVec2(90.0F, 28.0F));
            ImGui::PopStyleColor(3);
            if (stopClicked) {
                stopPlayMode();
            }
        }

        ImGui::End();
    }

    static bool inputTextString(
        const char* label, std::string& value, float width = 0.0F)
    {
        char buffer[128]{};
        std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
        if (width > 0.0F) {
            ImGui::SetNextItemWidth(width);
        }
        if (!ImGui::InputText(label, buffer, sizeof(buffer))) {
            return false;
        }
        value = buffer;
        return true;
    }

    static int findNameIndex(
        const std::string& name, const char* const* names, int count)
    {
        for (int index = 0; index < count; ++index) {
            if (name == names[index]) {
                return index;
            }
        }
        return 0;
    }

    static const char* getActionName(ActionType type)
    {
        constexpr const char* names[] = {
            "Apply Force", "Set Velocity", "Teleport", "Spawn Entity",
            "Destroy Entity", "Set Variable", "Broadcast Signal",
            "Parent To", "Unparent", "Shift Origin", "Move Grid Item",
            "Generate Chunk"};
        const int index = static_cast<int>(type);
        return index >= 0 && index < IM_ARRAYSIZE(names)
            ? names[index]
            : "Unknown Action";
    }

    static const char* getOperatorName(Operator op)
    {
        constexpr const char* names[] = {
            "equals", "does not equal", "is greater than", "is less than"};
        const int index = static_cast<int>(op);
        return index >= 0 && index < IM_ARRAYSIZE(names)
            ? names[index]
            : "?";
    }

    static bool renderCascadeCard(
        const char* id,
        const char* heading,
        const std::string& summary,
        const ImVec4& accent,
        const ImVec2& size,
        bool selected)
    {
        const ImVec4 background(
            accent.x * 0.20F,
            accent.y * 0.20F,
            accent.z * 0.20F,
            0.96F);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
        ImGui::PushStyleColor(ImGuiCol_Border, accent);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, selected ? 3.0F : 1.5F);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0F);
        ImGui::BeginChild(
            id,
            size,
            true,
            ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse);

        ImGui::TextColored(accent, "%s", heading);
        ImGui::TextWrapped("%s", summary.c_str());
        const bool clicked =
            ImGui::IsWindowHovered() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left);

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        return clicked;
    }

    static void drawCascadeConnector(float indent, float cardWidth)
    {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float centerX = start.x + indent + cardWidth * 0.5F;
        constexpr float connectorHeight = 22.0F;
        constexpr float arrowSize = 4.0F;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImU32 color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        drawList->AddLine(
            ImVec2(centerX, start.y + 2.0F),
            ImVec2(centerX, start.y + connectorHeight - arrowSize),
            color,
            1.5F);
        drawList->AddTriangleFilled(
            ImVec2(centerX - arrowSize, start.y + connectorHeight - arrowSize),
            ImVec2(centerX + arrowSize, start.y + connectorHeight - arrowSize),
            ImVec2(centerX, start.y + connectorHeight),
            color);
        ImGui::Dummy(ImVec2(1.0F, connectorHeight + 2.0F));
    }

    void renderLogicDomainMasterDetail(
        std::vector<LogicRule>& rules,
        const char* const* triggerNames,
        int triggerCount,
        int domainIndex)
    {
        constexpr const char* actionNames[] = {
            "Apply Force", "Set Velocity", "Teleport", "Spawn Entity",
            "Destroy Entity", "Set Variable", "Broadcast Signal",
            "Parent To", "Unparent", "Shift Origin", "Move Grid Item",
            "Generate Chunk"};
        constexpr const char* targetNames[] = {
            "Self", "Parent", "Children", "Tagged"};
        constexpr const char* operatorNames[] = {
            "Equals", "Not Equals", "Greater", "Less"};
        constexpr const char* keyNames[] = {
            "Space", "W", "A", "S", "D", "E", "Q", "Up", "Down",
            "Left", "Right"};
        constexpr int keyCodes[] = {
            GLFW_KEY_SPACE, GLFW_KEY_W, GLFW_KEY_A, GLFW_KEY_S,
            GLFW_KEY_D, GLFW_KEY_E, GLFW_KEY_Q, GLFW_KEY_UP,
            GLFW_KEY_DOWN, GLFW_KEY_LEFT, GLFW_KEY_RIGHT};
        constexpr const char* mouseButtonNames[] = {
            "Left", "Right", "Middle"};
        constexpr int mouseButtonCodes[] = {
            GLFW_MOUSE_BUTTON_LEFT, GLFW_MOUSE_BUTTON_RIGHT,
            GLFW_MOUSE_BUTTON_MIDDLE};

        constexpr ImVec4 triggerColor(0.81F, 0.49F, 0.17F, 1.0F);
        constexpr ImVec4 conditionColor(0.23F, 0.51F, 0.70F, 1.0F);
        constexpr ImVec4 actionColor(0.29F, 0.62F, 0.37F, 1.0F);

        if (selectedLogicDomain_ != domainIndex) {
            selectedLogicDomain_ = domainIndex;
            selectedRuleIndex_ = -1;
            selectedLogicCardType_ = LogicCardType::NONE;
            selectedLogicItemIndex_ = -1;
        }
        if (selectedRuleIndex_ >= static_cast<int>(rules.size())) {
            selectedRuleIndex_ = -1;
            selectedLogicCardType_ = LogicCardType::NONE;
            selectedLogicItemIndex_ = -1;
        }

        ImGui::Columns(2, "LogicEditorLayout", true);
        ImGui::SetColumnWidth(0, 320.0F);

        // LEFT: a scrollable, read-only cascade. Editing lives exclusively in
        // the property pane so cards stay compact and immediately scannable.
        ImGui::BeginChild("CascadeEditor", ImVec2(0.0F, 0.0F), false);
        ImGui::TextDisabled("LINEAR CASCADE");
        ImGui::Separator();

        for (std::size_t ruleIndex = 0; ruleIndex < rules.size(); ++ruleIndex) {
            LogicRule& rule = rules[ruleIndex];
            ImGui::PushID(static_cast<int>(ruleIndex));

            const bool triggerSelected =
                selectedRuleIndex_ == static_cast<int>(ruleIndex) &&
                selectedLogicCardType_ == LogicCardType::TRIGGER;
            if (renderCascadeCard(
                    "TriggerCard",
                    "TRIGGER",
                    "WHEN: " + rule.eventTriggerName,
                    triggerColor,
                    ImVec2(280.0F, 62.0F),
                    triggerSelected)) {
                selectedRuleIndex_ = static_cast<int>(ruleIndex);
                selectedLogicCardType_ = LogicCardType::TRIGGER;
                selectedLogicItemIndex_ = -1;
            }

            if (!rule.conditions.empty() || !rule.actions.empty()) {
                drawCascadeConnector(0.0F, 280.0F);
            }

            for (std::size_t conditionIndex = 0;
                 conditionIndex < rule.conditions.size();
                 ++conditionIndex) {
                const Condition& condition = rule.conditions[conditionIndex];
                ImGui::PushID(static_cast<int>(conditionIndex));
                ImGui::Indent(20.0F);
                char valueText[32]{};
                std::snprintf(
                    valueText, sizeof(valueText), "%.3g",
                    condition.targetValue);
                const std::string summary =
                    "AND: " + condition.varName + " " +
                    getOperatorName(condition.op) + " " +
                    valueText;
                const bool conditionSelected =
                    selectedRuleIndex_ == static_cast<int>(ruleIndex) &&
                    selectedLogicCardType_ == LogicCardType::CONDITION &&
                    selectedLogicItemIndex_ ==
                        static_cast<int>(conditionIndex);
                if (renderCascadeCard(
                        "ConditionCard",
                        "CONDITION",
                        summary,
                        conditionColor,
                        ImVec2(260.0F, 58.0F),
                        conditionSelected)) {
                    selectedRuleIndex_ = static_cast<int>(ruleIndex);
                    selectedLogicCardType_ = LogicCardType::CONDITION;
                    selectedLogicItemIndex_ =
                        static_cast<int>(conditionIndex);
                }
                ImGui::Unindent(20.0F);
                ImGui::PopID();

                const bool hasNextCard =
                    conditionIndex + 1 < rule.conditions.size() ||
                    !rule.actions.empty();
                if (hasNextCard) {
                    drawCascadeConnector(20.0F, 260.0F);
                }
            }

            for (std::size_t actionIndex = 0;
                 actionIndex < rule.actions.size();
                 ++actionIndex) {
                const Action& action = rule.actions[actionIndex];
                ImGui::PushID(static_cast<int>(actionIndex) + 10000);
                ImGui::Indent(40.0F);
                const std::string summary =
                    std::string("THEN: ") + getActionName(action.type);
                const bool actionSelected =
                    selectedRuleIndex_ == static_cast<int>(ruleIndex) &&
                    selectedLogicCardType_ == LogicCardType::ACTION &&
                    selectedLogicItemIndex_ == static_cast<int>(actionIndex);
                if (renderCascadeCard(
                        "ActionCard",
                        "ACTION",
                        summary,
                        actionColor,
                        ImVec2(240.0F, 58.0F),
                        actionSelected)) {
                    selectedRuleIndex_ = static_cast<int>(ruleIndex);
                    selectedLogicCardType_ = LogicCardType::ACTION;
                    selectedLogicItemIndex_ = static_cast<int>(actionIndex);
                }
                ImGui::Unindent(40.0F);
                ImGui::PopID();

                if (actionIndex + 1 < rule.actions.size()) {
                    drawCascadeConnector(40.0F, 240.0F);
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::PopID();
        }

        if (ImGui::Button(u8"\u2795 Add New Rule")) {
            LogicRule rule;
            rule.eventTriggerName = triggerNames[0];
            rules.push_back(std::move(rule));
            selectedRuleIndex_ = static_cast<int>(rules.size()) - 1;
            selectedLogicCardType_ = LogicCardType::TRIGGER;
            selectedLogicItemIndex_ = -1;
        }
        ImGui::EndChild();

        ImGui::NextColumn();

        // RIGHT: one focused form for the currently selected card.
        ImGui::BeginChild("CascadeProperties", ImVec2(0.0F, 0.0F), false);
        ImGui::TextDisabled("CARD PROPERTIES");
        ImGui::Separator();

        if (selectedRuleIndex_ < 0 ||
            selectedRuleIndex_ >= static_cast<int>(rules.size()) ||
            selectedLogicCardType_ == LogicCardType::NONE) {
            ImGui::TextDisabled(
                "Click a trigger, condition, or action card to edit it.");
            ImGui::EndChild();
            ImGui::Columns(1);
            return;
        }

        LogicRule& rule = rules[static_cast<std::size_t>(selectedRuleIndex_)];

        if (selectedLogicCardType_ == LogicCardType::TRIGGER) {
            ImGui::TextColored(triggerColor, "TRIGGER SETTINGS");
            ImGui::Spacing();

            int triggerIndex = findNameIndex(
                rule.eventTriggerName, triggerNames, triggerCount);
            if (ImGui::Combo(
                    "Trigger Event", &triggerIndex, triggerNames,
                    triggerCount)) {
                rule.eventTriggerName = triggerNames[triggerIndex];
                rule.keyWasDown = false;
                rule.timerAccumulator = 0.0F;
            }

            if (rule.eventTriggerName == "Key Pressed" ||
                rule.eventTriggerName == "Key Held" ||
                rule.eventTriggerName == "Key Released") {
                int keyIndex = 0;
                for (int index = 0; index < IM_ARRAYSIZE(keyCodes); ++index) {
                    if (rule.targetKey == keyCodes[index]) {
                        keyIndex = index;
                        break;
                    }
                }
                if (ImGui::Combo(
                        "Key", &keyIndex, keyNames,
                        IM_ARRAYSIZE(keyNames))) {
                    rule.targetKey = keyCodes[keyIndex];
                }
            } else if (rule.eventTriggerName == "Mouse Pressed") {
                int buttonIndex = 0;
                for (int index = 0;
                     index < IM_ARRAYSIZE(mouseButtonCodes);
                     ++index) {
                    if (rule.targetKey == mouseButtonCodes[index]) {
                        buttonIndex = index;
                        break;
                    }
                }
                if (ImGui::Combo(
                        "Mouse Button", &buttonIndex, mouseButtonNames,
                        IM_ARRAYSIZE(mouseButtonNames))) {
                    rule.targetKey = mouseButtonCodes[buttonIndex];
                }
            } else if (rule.eventTriggerName == "Timer") {
                ImGui::DragFloat(
                    "Interval (seconds)", &rule.timerInterval, 0.05F,
                    0.001F, 3600.0F, "%.3f");
            } else if (rule.eventTriggerName != "On Play Start" &&
                       rule.eventTriggerName != "Every Frame") {
                inputTextString("Target Filter Tag/Name", rule.targetStr);
            }

            if (ImGui::Button(u8"\u2795 Add Condition")) {
                rule.conditions.push_back(Condition{});
                selectedLogicCardType_ = LogicCardType::CONDITION;
                selectedLogicItemIndex_ =
                    static_cast<int>(rule.conditions.size()) - 1;
            }
            ImGui::SameLine();
            if (ImGui::Button(u8"\u2795 Add Action")) {
                rule.actions.push_back(Action{});
                selectedLogicCardType_ = LogicCardType::ACTION;
                selectedLogicItemIndex_ =
                    static_cast<int>(rule.actions.size()) - 1;
            }

            ImGui::Spacing();
            ImGui::PushStyleColor(
                ImGuiCol_Button, ImVec4(0.62F, 0.12F, 0.12F, 1.0F));
            const bool deleteRule = ImGui::Button("Delete Entire Rule");
            ImGui::PopStyleColor();
            if (deleteRule) {
                rules.erase(rules.begin() + selectedRuleIndex_);
                selectedRuleIndex_ = -1;
                selectedLogicCardType_ = LogicCardType::NONE;
                selectedLogicItemIndex_ = -1;
            }
        } else if (selectedLogicCardType_ == LogicCardType::CONDITION) {
            if (selectedLogicItemIndex_ < 0 ||
                selectedLogicItemIndex_ >=
                    static_cast<int>(rule.conditions.size())) {
                selectedLogicCardType_ = LogicCardType::TRIGGER;
                selectedLogicItemIndex_ = -1;
            } else {
                Condition& condition = rule.conditions[
                    static_cast<std::size_t>(selectedLogicItemIndex_)];
                ImGui::TextColored(conditionColor, "CONDITION SETTINGS");
                ImGui::Spacing();
                inputTextString("Variable Name", condition.varName);
                int operatorIndex = static_cast<int>(condition.op);
                if (ImGui::Combo(
                        "Comparison", &operatorIndex, operatorNames,
                        IM_ARRAYSIZE(operatorNames))) {
                    condition.op = static_cast<Operator>(operatorIndex);
                }
                ImGui::InputFloat(
                    "Target Value", &condition.targetValue, 0.1F, 1.0F, "%.3f");

                ImGui::Spacing();
                ImGui::PushStyleColor(
                    ImGuiCol_Button, ImVec4(0.62F, 0.12F, 0.12F, 1.0F));
                const bool deleteCondition =
                    ImGui::Button("Delete Condition");
                ImGui::PopStyleColor();
                if (deleteCondition) {
                    rule.conditions.erase(
                        rule.conditions.begin() + selectedLogicItemIndex_);
                    selectedLogicCardType_ = LogicCardType::TRIGGER;
                    selectedLogicItemIndex_ = -1;
                }
            }
        } else if (selectedLogicCardType_ == LogicCardType::ACTION) {
            if (selectedLogicItemIndex_ < 0 ||
                selectedLogicItemIndex_ >=
                    static_cast<int>(rule.actions.size())) {
                selectedLogicCardType_ = LogicCardType::TRIGGER;
                selectedLogicItemIndex_ = -1;
            } else {
                Action& action = rule.actions[
                    static_cast<std::size_t>(selectedLogicItemIndex_)];
                ImGui::TextColored(actionColor, "ACTION SETTINGS");
                ImGui::Spacing();

                int actionIndex = static_cast<int>(action.type);
                if (ImGui::Combo(
                        "Behavior", &actionIndex, actionNames,
                        IM_ARRAYSIZE(actionNames))) {
                    action.type = static_cast<ActionType>(actionIndex);
                }
                int targetIndex = static_cast<int>(action.target);
                if (ImGui::Combo(
                        "Apply To", &targetIndex, targetNames,
                        IM_ARRAYSIZE(targetNames))) {
                    action.target = static_cast<TargetType>(targetIndex);
                }
                if (action.target == TargetType::TAGGED) {
                    inputTextString("Target Tag", action.targetTag);
                }

                const bool usesVector =
                    action.type == ActionType::APPLY_FORCE ||
                    action.type == ActionType::SET_VELOCITY ||
                    action.type == ActionType::TELEPORT ||
                    action.type == ActionType::SPAWN_ENTITY ||
                    action.type == ActionType::SHIFT_ORIGIN ||
                    action.type == ActionType::MOVE_GRID_ITEM;
                const bool usesString =
                    action.type == ActionType::SPAWN_ENTITY ||
                    action.type == ActionType::SET_VAR ||
                    action.type == ActionType::BROADCAST_SIGNAL ||
                    action.type == ActionType::PARENT_TO ||
                    action.type == ActionType::GENERATE_CHUNK;
                const bool usesFloat =
                    action.type == ActionType::APPLY_FORCE ||
                    action.type == ActionType::SET_VAR;

                if (usesVector) {
                    ImGui::SeparatorText("Vector / Axis");
                    ImGui::DragFloat3(
                        "X / Y / Z", glm::value_ptr(action.vecData), 0.05F,
                        -100000.0F, 100000.0F, "%.3f");
                    if (ImGui::SmallButton("+X")) {
                        action.vecData = glm::vec3(1.0F, 0.0F, 0.0F);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("+Y")) {
                        action.vecData = glm::vec3(0.0F, 1.0F, 0.0F);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("+Z")) {
                        action.vecData = glm::vec3(0.0F, 0.0F, 1.0F);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("-X")) {
                        action.vecData = glm::vec3(-1.0F, 0.0F, 0.0F);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("-Y")) {
                        action.vecData = glm::vec3(0.0F, -1.0F, 0.0F);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("-Z")) {
                        action.vecData = glm::vec3(0.0F, 0.0F, -1.0F);
                    }
                }
                if (usesString) {
                    const char* label =
                        action.type == ActionType::BROADCAST_SIGNAL
                            ? "Signal / Script Message"
                            : action.type == ActionType::PARENT_TO
                                ? "Parent Tag or Name"
                                : action.type == ActionType::SET_VAR
                                    ? "Variable Name"
                                    : action.type == ActionType::SPAWN_ENTITY
                                        ? "Spawned Entity Name"
                                        : "Behavior Data";
                    inputTextString(label, action.strData);
                }
                if (usesFloat) {
                    const char* label =
                        action.type == ActionType::APPLY_FORCE
                            ? "Force Strength"
                            : "Variable Value";
                    ImGui::InputFloat(
                        label, &action.floatData, 0.1F, 1.0F, "%.3f");
                }

                ImGui::Spacing();
                ImGui::PushStyleColor(
                    ImGuiCol_Button, ImVec4(0.62F, 0.12F, 0.12F, 1.0F));
                const bool deleteAction = ImGui::Button("Delete Action");
                ImGui::PopStyleColor();
                if (deleteAction) {
                    rule.actions.erase(
                        rule.actions.begin() + selectedLogicItemIndex_);
                    selectedLogicCardType_ = LogicCardType::TRIGGER;
                    selectedLogicItemIndex_ = -1;
                }
            }
        }

        ImGui::EndChild();
        ImGui::Columns(1);
    }

    void renderLogicEditor()
    {
        ImGui::Begin("Logic Editor", &showLogicEditor_);
        const bool validSelection = selectedEntityIndex >= 0 &&
            static_cast<std::size_t>(selectedEntityIndex) < sceneEntities.size();
        if (!validSelection) {
            ImGui::TextDisabled("Select an entity to edit its logic.");
            ImGui::End();
            return;
        }

        Entity& entity =
            sceneEntities[static_cast<std::size_t>(selectedEntityIndex)];
        ImGui::Text("Universal Logic: %s", entity.name.c_str());
        ImGui::BeginDisabled(currentState == EngineState::PLAY);

        constexpr const char* lifecycleTriggers[] = {
            "On Play Start", "On Destroy", "On Chunk Load"};
        constexpr const char* inputTriggers[] = {
            "Key Pressed", "Key Held", "Key Released", "Mouse Pressed"};
        constexpr const char* physicsTriggers[] = {
            "Collision Enter", "Collision Stay", "Raycast Hit"};
        constexpr const char* brainTriggers[] = {
            "Every Frame", "Timer", "State Is"};
        constexpr const char* worldTriggers[] = {
            "Signal Received", "Origin Shifted", "Chunk Generated"};

        if (ImGui::BeginTabBar("UniversalLogic")) {
            if (ImGui::BeginTabItem(u8"\U0001F331 Lifecycle")) {
                renderLogicDomainMasterDetail(
                    entity.logicLifecycle, lifecycleTriggers,
                    IM_ARRAYSIZE(lifecycleTriggers), 0);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(u8"\U0001F3AE Input")) {
                renderLogicDomainMasterDetail(
                    entity.logicInput, inputTriggers,
                    IM_ARRAYSIZE(inputTriggers), 1);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(u8"\U0001F4A5 Physics")) {
                renderLogicDomainMasterDetail(
                    entity.logicPhysics, physicsTriggers,
                    IM_ARRAYSIZE(physicsTriggers), 2);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(u8"\U0001F9E0 Brain")) {
                renderLogicDomainMasterDetail(
                    entity.logicBrain, brainTriggers,
                    IM_ARRAYSIZE(brainTriggers), 3);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(u8"\U0001F30D World")) {
                renderLogicDomainMasterDetail(
                    entity.logicWorld, worldTriggers,
                    IM_ARRAYSIZE(worldTriggers), 4);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndDisabled();
        ImGui::End();
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
        renderDockSpaceHost();
        renderPlayToolbar();

        // ENGINE DEBUG -----------------------------------------------------
        // WorkingSetSize is the physical RAM currently resident for this
        // process. This is a live budget indicator, not a forced allocation
        // cap, so the editor can warn before memory pressure becomes severe.
        if (showProfiler_) {
        ImGui::Begin("Memory Profiler", &showProfiler_);
        const float currentRamMB = getCurrentRamUsageMB();
        const bool memoryBudgetExceeded = currentRamMB > 512.0F;
        if (memoryBudgetExceeded) {
            ImGui::PushStyleColor(
                ImGuiCol_Text, ImVec4(1.0F, 0.15F, 0.15F, 1.0F));
        }
        ImGui::Text("RAM Usage: %.2f MB / 512 MB", currentRamMB);
        ImGui::Text(
            "Mode: %s",
            currentState == EngineState::EDIT ? "EDIT" : "PLAY");
        if (memoryBudgetExceeded) {
            ImGui::PopStyleColor();
        }
        ImGui::End();
        }

        // SCENE HIERARCHY ---------------------------------------------------
        // This panel owns creation and selection. Entities start empty; attach
        // an OBJ from the Inspector to give one visible geometry.
        if (showHierarchy_) {
        ImGui::Begin("Scene Hierarchy", &showHierarchy_);

        ImGui::BeginDisabled(currentState == EngineState::PLAY);

        if (ImGui::Button("Spawn Default Cube")) {
            if (!defaultCubeModel_) {
                // All default cubes share one immutable VAO/VBO/EBO and one
                // one-pixel texture, minimizing both RAM and VRAM use.
                defaultCubeModel_ = createDefaultCubeModel();
            }

            const std::size_t newIndex = sceneEntities.size();
            Entity cube;
            cube.uuid = createEntityUuid();
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
            entity.uuid = createEntityUuid();
            entity.name = "Entity " + std::to_string(newIndex + 1);
            entity.position.x = static_cast<float>(newIndex % 5) * 1.5F;
            entity.position.z = -static_cast<float>(newIndex / 5) * 1.5F;

            sceneEntities.push_back(std::move(entity));
            createPhysicsBody(sceneEntities.back());
            selectedEntityIndex = static_cast<int>(newIndex);
        }

        ImGui::EndDisabled();

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
        }

        // INSPECTOR ---------------------------------------------------------
        // glm::value_ptr exposes the three contiguous float components ImGui's
        // DragFloat3 expects. Changes are therefore visible in the next draw.
        if (showInspector_) {
        ImGui::Begin("Inspector", &showInspector_);

        const bool hasValidSelection =
            selectedEntityIndex >= 0 &&
            static_cast<std::size_t>(selectedEntityIndex) < sceneEntities.size();

        if (hasValidSelection) {
            Entity& selectedEntity =
                sceneEntities[static_cast<std::size_t>(selectedEntityIndex)];

            // inputTextString copies through a fixed mutable char buffer and
            // writes the edited value back into std::string safely.
            inputTextString(
                "Name##EntityName", selectedEntity.name, 220.0F);
            ImGui::TextDisabled("UUID: %s", selectedEntity.uuid.c_str());
            ImGui::Separator();
            ImGui::BeginDisabled(currentState == EngineState::PLAY);
            inputTextString("Tag", selectedEntity.tag, 180.0F);
            inputTextString("Parent UUID", selectedEntity.parentUuid, 180.0F);
            inputTextString("State", selectedEntity.currentState, 180.0F);
            if (ImGui::Checkbox("Active", &selectedEntity.isActive) &&
                selectedEntity.rigidBody != nullptr) {
                selectedEntity.rigidBody->setIsActive(
                    selectedEntity.isActive &&
                    selectedEntity.parentUuid.empty());
            }
            ImGui::Checkbox("Visible", &selectedEntity.isVisible);

            if (ImGui::Checkbox("Dynamic", &selectedEntity.isDynamic)) {
                rebuildPhysicsBody(selectedEntity);
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
            if (transformChanged) {
                updateLocalOffsetFromWorld(selectedEntity);
            }
            if (transformChanged && selectedEntity.rigidBody != nullptr &&
                selectedEntity.parentUuid.empty()) {
                // Inspector edits teleport the body once. From the next physics
                // step onward, the rigid body is authoritative again.
                selectedEntity.rigidBody->setTransform(
                    makePhysicsTransform(selectedEntity));
                selectedEntity.rigidBody->setIsSleeping(false);
            }

            if (ImGui::DragFloat3(
                    "Scale", glm::value_ptr(selectedEntity.scale), 0.05F) &&
                selectedEntity.rigidBody != nullptr &&
                selectedEntity.collider != nullptr) {
                // The visual scale is the cube's full size; ReactPhysics3D's
                // BoxShape expects half-extents. Rebuilding also refreshes the
                // broad-phase bounds and any cached contacts immediately.
                const reactphysics3d::Transform editedTransform =
                    makePhysicsTransform(selectedEntity);
                rebuildPhysicsBody(selectedEntity, &editedTransform);
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
            ImGui::EndDisabled();
        } else {
            selectedEntityIndex = -1;
            ImGui::TextDisabled("Select an entity to edit its transform.");
        }

        ImGui::End();
        }

        // Five compact domain buckets replace a general node graph. Each tab
        // is a top-down WHEN -> AND -> THEN flowchart backed by contiguous
        // vectors, so authoring remains readable without graph allocations.
        if (showLogicEditor_) {
            renderLogicEditor();
        }

        // Draw and handle the selected entity's transform arrows after the
        // panels have been built, but before ImGui generates its draw data.
        renderTransformGizmo();
        handleViewportPicking();

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
        sceneBackup.clear();
        signalQueue_.clear();
        pendingSpawns_.clear();
        parentSyncState_.clear();
        defaultCubeModel_.reset();
        selectedEntityIndex = -1;
        currentState = EngineState::EDIT;

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
    SceneCollisionListener collisionListener_;

    float deltaTime_ = 0.0F;
    float lastFrame_ = 0.0F;
    float lastMouseX_ = 0.0F;
    float lastMouseY_ = 0.0F;

    std::string modelLoadMessage_;
    std::vector<std::string> signalQueue_;
    std::vector<Entity> pendingSpawns_;
    std::vector<std::uint8_t> parentSyncState_;
    std::uint64_t nextEntityUuid_ = 1;

    bool glfwInitialized_ = false;
    bool imguiContextCreated_ = false;
    bool imguiGlfwInitialized_ = false;
    bool imguiOpenGLInitialized_ = false;
    bool mouseLookActive_ = false;
    bool firstMouseSample_ = true;
    bool modelLoadFailed_ = false;
    bool lifecyclePending_ = false;
    bool showHierarchy_ = true;
    bool showInspector_ = true;
    bool showLogicEditor_ = true;
    bool showProfiler_ = true;
    bool resetDockLayout_ = false;
    int selectedLogicDomain_ = -1;
    int selectedRuleIndex_ = -1;
    LogicCardType selectedLogicCardType_ = LogicCardType::NONE;
    int selectedLogicItemIndex_ = -1;
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
