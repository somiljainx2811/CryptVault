// Test helper: a bare GLFW window that prints whatever is dropped on it.
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstdlib>
static void OnDrop(GLFWwindow*, int count, const char** paths) {
    for (int i = 0; i < count; ++i) std::printf("DROPPED: %s\n", paths[i]);
    std::fflush(stdout);
}
int main(int argc, char** argv) {
    int x = argc > 1 ? std::atoi(argv[1]) : -1, y = argc > 2 ? std::atoi(argv[2]) : 0;
    if (!glfwInit()) { std::printf("glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* w = glfwCreateWindow(320, 320, "droptarget", nullptr, nullptr);
    if (!w) { std::printf("window failed\n"); return 1; }
    if (x >= 0 && glfwGetPlatform() == GLFW_PLATFORM_X11) glfwSetWindowPos(w, x, y);
    glfwSetDropCallback(w, OnDrop);
    std::printf("droptarget ready on %s\n", glfwGetPlatform() == GLFW_PLATFORM_WAYLAND ? "wayland" : "x11");
    std::fflush(stdout);
    while (!glfwWindowShouldClose(w)) glfwWaitEventsTimeout(0.05);
    return 0;
}
