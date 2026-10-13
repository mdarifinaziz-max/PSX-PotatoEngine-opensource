#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

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
        previousFrameTime_ = Clock::now();

        while (!glfwWindowShouldClose(window_)) {
            pollInputs();
            updateEngineState();
            renderUI();
            glfwSwapBuffers(window_);
        }
    }

private:
    using Clock = std::chrono::steady_clock;

    void initializeWindow()
    {
        glfwSetErrorCallback(glfwErrorCallback);

        if (glfwInit() != GLFW_TRUE) {
            throw std::runtime_error("Failed to initialize GLFW");
        }
        glfwInitialized_ = true;

        // Request only OpenGL 3.1. No 3.2+ core-profile or modern feature
        // requirement is introduced, preserving compatibility with old iGPUs.
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);

        window_ = glfwCreateWindow(1280, 720, "PS2-Style Engine", nullptr, nullptr);
        if (window_ == nullptr) {
            throw std::runtime_error("Failed to create the GLFW window");
        }

        glfwMakeContextCurrent(window_);
        glfwSwapInterval(1); // VSync: do not spin on unconstrained blank frames.

        if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
            throw std::runtime_error("Failed to load OpenGL functions with GLAD");
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

        // GLSL 1.40 corresponds to OpenGL 3.1.
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
    }

    void updateEngineState()
    {
        const Clock::time_point now = Clock::now();
        deltaTimeSeconds_ = std::chrono::duration<float>(now - previousFrameTime_).count();
        previousFrameTime_ = now;

        // Future simulation and game-state updates go here.
        (void)deltaTimeSeconds_;
    }

    void renderUI()
    {
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(window_, &framebufferWidth, &framebufferHeight);

        glViewport(0, 0, framebufferWidth, framebufferHeight);
        glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("Engine Debug");
        ImGui::End();

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
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
    Clock::time_point previousFrameTime_{};
    float deltaTimeSeconds_ = 0.0F;
    bool glfwInitialized_ = false;
    bool imguiContextCreated_ = false;
    bool imguiGlfwInitialized_ = false;
    bool imguiOpenGLInitialized_ = false;
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

