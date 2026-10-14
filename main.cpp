#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

// This is deliberately a simple engine/UI variable for now. ImGui edits it
// directly, and the world renderer consumes it when building the model matrix.
float rotationSpeed = 1.0F;

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
in vec3 aColor;

out vec3 vertexColor;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

void main()
{
    // Matrix multiplication reads right-to-left:
    // model/local space -> world space -> camera space -> clip space.
    gl_Position = uProjection * uView * uModel * vec4(aPosition, 1.0);
    vertexColor = aColor;
}
)GLSL";

constexpr const char* kFragmentShaderSource = R"GLSL(
#version 140

in vec3 vertexColor;
out vec4 fragmentColor;

void main()
{
    // The RGB value is interpolated across each triangle automatically.
    fragmentColor = vec4(vertexColor, 1.0);
}
)GLSL";

// Vertex format: position X/Y/Z followed by color R/G/B.
//
// This intentionally uses 36 vertices (two triangles for each of six faces)
// instead of an index buffer. It is a little repetitive, but makes the first
// renderer very easy to inspect. A mesh system can introduce EBOs later.
constexpr float kCubeVertices[] = {
    // Back face
    -0.5F, -0.5F, -0.5F,  1.0F, 0.0F, 0.0F,
     0.5F,  0.5F, -0.5F,  0.0F, 1.0F, 0.0F,
     0.5F, -0.5F, -0.5F,  0.0F, 0.0F, 1.0F,
     0.5F,  0.5F, -0.5F,  0.0F, 1.0F, 0.0F,
    -0.5F, -0.5F, -0.5F,  1.0F, 0.0F, 0.0F,
    -0.5F,  0.5F, -0.5F,  1.0F, 1.0F, 0.0F,

    // Front face
    -0.5F, -0.5F,  0.5F,  1.0F, 0.0F, 1.0F,
     0.5F, -0.5F,  0.5F,  0.0F, 1.0F, 1.0F,
     0.5F,  0.5F,  0.5F,  1.0F, 1.0F, 1.0F,
     0.5F,  0.5F,  0.5F,  1.0F, 1.0F, 1.0F,
    -0.5F,  0.5F,  0.5F,  0.2F, 0.6F, 1.0F,
    -0.5F, -0.5F,  0.5F,  1.0F, 0.0F, 1.0F,

    // Left face
    -0.5F,  0.5F,  0.5F,  0.2F, 0.6F, 1.0F,
    -0.5F,  0.5F, -0.5F,  1.0F, 1.0F, 0.0F,
    -0.5F, -0.5F, -0.5F,  1.0F, 0.0F, 0.0F,
    -0.5F, -0.5F, -0.5F,  1.0F, 0.0F, 0.0F,
    -0.5F, -0.5F,  0.5F,  1.0F, 0.0F, 1.0F,
    -0.5F,  0.5F,  0.5F,  0.2F, 0.6F, 1.0F,

    // Right face
     0.5F,  0.5F,  0.5F,  1.0F, 1.0F, 1.0F,
     0.5F, -0.5F, -0.5F,  0.0F, 0.0F, 1.0F,
     0.5F,  0.5F, -0.5F,  0.0F, 1.0F, 0.0F,
     0.5F, -0.5F, -0.5F,  0.0F, 0.0F, 1.0F,
     0.5F,  0.5F,  0.5F,  1.0F, 1.0F, 1.0F,
     0.5F, -0.5F,  0.5F,  0.0F, 1.0F, 1.0F,

    // Bottom face
    -0.5F, -0.5F, -0.5F,  1.0F, 0.0F, 0.0F,
     0.5F, -0.5F, -0.5F,  0.0F, 0.0F, 1.0F,
     0.5F, -0.5F,  0.5F,  0.0F, 1.0F, 1.0F,
     0.5F, -0.5F,  0.5F,  0.0F, 1.0F, 1.0F,
    -0.5F, -0.5F,  0.5F,  1.0F, 0.0F, 1.0F,
    -0.5F, -0.5F, -0.5F,  1.0F, 0.0F, 0.0F,

    // Top face
    -0.5F,  0.5F, -0.5F,  1.0F, 1.0F, 0.0F,
     0.5F,  0.5F,  0.5F,  1.0F, 1.0F, 1.0F,
     0.5F,  0.5F, -0.5F,  0.0F, 1.0F, 0.0F,
     0.5F,  0.5F,  0.5F,  1.0F, 1.0F, 1.0F,
    -0.5F,  0.5F, -0.5F,  1.0F, 1.0F, 0.0F,
    -0.5F,  0.5F,  0.5F,  0.2F, 0.6F, 1.0F,
};

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

            // GLSL 1.40 has no core layout(location) qualifier. Bind the
            // position and color inputs to slots 0 and 1 before linking.
            glBindAttribLocation(shaderProgram_, 0, "aPosition");
            glBindAttribLocation(shaderProgram_, 1, "aColor");
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
        if (modelLocation_ < 0 || viewLocation_ < 0 || projectionLocation_ < 0) {
            throw std::runtime_error("Failed to locate an MVP shader uniform");
        }

        // The VBO owns the vertex bytes. The VAO remembers how those bytes map
        // onto shader inputs, so this setup happens only once at startup.
        glGenVertexArrays(1, &cubeVao_);
        glGenBuffers(1, &cubeVbo_);

        glBindVertexArray(cubeVao_);
        glBindBuffer(GL_ARRAY_BUFFER, cubeVbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kCubeVertices), kCubeVertices, GL_STATIC_DRAW);

        constexpr GLsizei stride = 6 * static_cast<GLsizei>(sizeof(float));

        // Attribute 0 reads the first three floats as XYZ position.
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);

        // Attribute 1 skips XYZ, then reads the next three floats as RGB.
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(
            1,
            3,
            GL_FLOAT,
            GL_FALSE,
            stride,
            reinterpret_cast<const void*>(3 * sizeof(float)));

        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);

        // CAMERA MATH -------------------------------------------------------
        // The projection is static for this fixed-aspect window. The view is
        // rebuilt every frame later because the editor camera can move.
        projection_ = glm::perspective(
            glm::radians(45.0F),
            1280.0F / 720.0F,
            0.1F,
            100.0F);

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
        // glfwGetTime() is elapsed time in seconds. Multiplying by 45 degrees
        // per second gives smooth, frame-rate-independent rotation.
        const float angle = static_cast<float>(glfwGetTime()) *
                            rotationSpeed * glm::radians(45.0F);
        const glm::mat4 model = glm::rotate(
            glm::mat4(1.0F),
            angle,
            glm::normalize(glm::vec3(0.6F, 1.0F, 0.25F)));

        // Select the cube shader and upload its three transformation matrices.
        // GL_FALSE is correct because GLM already stores matrices in the
        // column-major layout expected by OpenGL.
        // Rebuild the view every frame because the editor camera can move.
        const glm::mat4 view = glm::lookAt(
            camera_.cameraPos,
            camera_.cameraPos + camera_.cameraFront,
            camera_.cameraUp);

        glUseProgram(shaderProgram_);
        glUniformMatrix4fv(modelLocation_, 1, GL_FALSE, glm::value_ptr(model));
        glUniformMatrix4fv(viewLocation_, 1, GL_FALSE, glm::value_ptr(view));
        glUniformMatrix4fv(projectionLocation_, 1, GL_FALSE, glm::value_ptr(projection_));

        glBindVertexArray(cubeVao_);
        glDrawArrays(GL_TRIANGLES, 0, 36);
        glBindVertexArray(0);
        glUseProgram(0);
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

        ImGui::Begin("Engine Debug");
        ImGui::SliderFloat("Cube Speed", &rotationSpeed, 0.0F, 10.0F);
        ImGui::End();

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    void renderFrame()
    {
        renderScene(); // First: render the cube into the color/depth buffers.
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

        // Delete graphics resources before destroying their OpenGL context.
        if (cubeVbo_ != 0) {
            glDeleteBuffers(1, &cubeVbo_);
            cubeVbo_ = 0;
        }
        if (cubeVao_ != 0) {
            glDeleteVertexArrays(1, &cubeVao_);
            cubeVao_ = 0;
        }
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
    GLuint cubeVao_ = 0;
    GLuint cubeVbo_ = 0;
    GLint modelLocation_ = -1;
    GLint viewLocation_ = -1;
    GLint projectionLocation_ = -1;

    glm::mat4 projection_{1.0F};

    CameraState camera_{};

    float deltaTime_ = 0.0F;
    float lastFrame_ = 0.0F;
    float lastMouseX_ = 0.0F;
    float lastMouseY_ = 0.0F;

    bool glfwInitialized_ = false;
    bool imguiContextCreated_ = false;
    bool imguiGlfwInitialized_ = false;
    bool imguiOpenGLInitialized_ = false;
    bool mouseLookActive_ = false;
    bool firstMouseSample_ = true;
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
