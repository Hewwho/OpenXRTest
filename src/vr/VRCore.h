#ifndef VR_VRCORE_H
#define VR_VRCORE_H

// needs to be included before openxr
#include <epoxy/wgl.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define SDL_MAIN_HANDLED

#include <SDL.h>

#include <vector>
#include <string>


class VRCore {
public:
    VRCore();
    ~VRCore();
    bool initVR();
    void runVR();

private:
    XrInstance m_instance;
    XrSession m_session;
    bool m_isSessionRunning;
    bool m_isSessionFocused;
    uint64_t m_systemId;
    XrSpace m_space;

    void createInstance();
    std::vector<const char *> getExtensions() const;
    void initSystem();
    void initSession();
    void initReferenceSpace();
    void handleStateChange(XrEventDataBuffer event);
    XrResult checkResult(const XrResult, const std::string) const;


    // SDL stuff
    void createWindow();


    // Rendering
    static const short VIEW_COUNT = 2;
    std::vector<XrView> m_views;
    XrViewConfigurationType m_viewConfigurationType{ XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
    std::vector<XrViewConfigurationView> m_configViews;
    XrEnvironmentBlendMode m_environmentBlendMode{ XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
    std::vector<std::vector<XrSwapchainImageOpenGLKHR>> m_images;
    std::vector<XrSwapchain> m_swapchains;
    uint32_t m_swapchainLength;

    void initRendering();
    void render();


    // GL stuff TODO move this out
    GLuint m_programId;
    GLuint m_vertexArrayId;
    GLuint m_vertexBufferId;
    GLuint m_emptyCubeIndexBufferId;
    GLuint m_filledCubeIndexBufferId;
    GLuint m_modelViewProjectionUniformId;
    GLuint m_vertexColorUniformId;
    std::vector<GLuint> m_frameBuffer;

    void initGL();


    // Cube stuff TODO move this out
    enum class CubeType {
        EMPTY,
        FILLED
    };

    typedef struct Cube {
        XrVector3f translation;
        XrQuaternionf rotation;
        XrVector3f scale;
        XrColor4f color;
        CubeType type;
    };

    std::vector<Cube> m_cubes;

    void drawCube(CubeType type);


    // Actions
    typedef struct Hand {
        XrPath path;
        XrSpace space;
        // Mostly an annoyance
        //XrAction vibrateAction;
        XrColor4f color = { 1.f, 1.f, 1.f, 1.f };
        XrVector3f scale = { 1.f, 1.f, 1.f };
        CubeType type = CubeType::EMPTY;

        float m_colorStartingAngle = -1;
        XrColor4f m_originalColor;
    };

    typedef struct InputActions {
        XrAction pose;
        XrAction place;
        XrAction expand;
        XrAction shrink;
        XrAction modifierXA;
        XrAction modifierYB;
        XrAction thumbstickX;
        XrAction thumbstickY;
    };

    std::vector<Hand> m_hands = { Hand(), Hand() };
    InputActions m_inputActions;
    XrActionSet m_actionSet;

    void pollActions();
    void initActions();
};

#endif //VR_VRCORE_H
