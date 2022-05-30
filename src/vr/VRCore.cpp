// TODO some refactoring (move out GL stuff), depth buffer, some lighting and maybe a bit of physics

#include "vr/VRCore.h"
#include "vr/XrMatrix4x4f.h"

#include "spdlog/spdlog.h"



VRCore::VRCore() {
    try {
        createWindow();

        createInstance();

        initSystem();
        initSession();

        initReferenceSpace();
        initActions();
        initRendering();

        initGL();
    }
    catch (std::runtime_error e) {
        VRCore::~VRCore();
        throw e;
    }
}

void VRCore::runVR() {
    XrResult pollResult;
    while (true) {
        do {
            XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
            event.next = nullptr;
            pollResult = xrPollEvent(m_instance, &event);
            if (pollResult == XR_SUCCESS) {
                switch (event.type) {
                    case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                        handleStateChange(event);

                        break;
                    }
                    case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
                        throw std::runtime_error("The instance is about to become unusable");

                        break;
                    }
                    default: {
                        char eventBuffer[XR_MAX_STRUCTURE_NAME_SIZE];
                        xrStructureTypeToString(m_instance, event.type, eventBuffer);
                        spdlog::info("OTHER EVENT: {}", eventBuffer);

                        break;
                    }
                }
            }
        } while (pollResult == XR_SUCCESS);

        if (m_isSessionRunning) {
            if (m_isSessionFocused) {
                pollActions();
            }
            render();
        }

    }

}

void VRCore::handleStateChange(XrEventDataBuffer event) {
    const XrEventDataSessionStateChanged &stateEvent = *reinterpret_cast<XrEventDataSessionStateChanged *>(&event);

    spdlog::info("SESSION STATE: {}", stateEvent.state);

    switch (stateEvent.state) {
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo sessionBeginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
            sessionBeginInfo.primaryViewConfigurationType = m_viewConfigurationType;
            checkResult(xrBeginSession(m_session, &sessionBeginInfo), "Beginning the session");
            m_isSessionRunning = true;

            break;
        }
        case XR_SESSION_STATE_VISIBLE: {
            m_isSessionFocused = false;

            break;
        }
        case XR_SESSION_STATE_FOCUSED: {
            m_isSessionFocused = true;

            break;
        }
        case XR_SESSION_STATE_STOPPING: {
            m_isSessionRunning = false;
            m_isSessionFocused = false;
            checkResult(xrEndSession(m_session), "Stopping the session");

            break;
        }
        case XR_SESSION_STATE_EXITING: {
            throw std::runtime_error("Improper session exit");
        }
    }
}

void VRCore::pollActions() {
    const XrActiveActionSet activeActionSet{ m_actionSet, XR_NULL_PATH };
    XrActionsSyncInfo syncInfo{ XR_TYPE_ACTIONS_SYNC_INFO };
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeActionSet;
    checkResult(xrSyncActions(m_session, &syncInfo), "Syncing actions");

    // Not enough buttons and no interface -> modifiers
    std::vector<bool> modifierXAs;
    std::vector<bool> modifierYBs;

    for (int handIndex = 0; handIndex < 2; handIndex++) {
        Hand hand = m_hands[handIndex];
        XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
        getInfo.subactionPath = hand.path;

        getInfo.action = m_inputActions.modifierXA;
        XrActionStateBoolean modifierXAClickState{ XR_TYPE_ACTION_STATE_BOOLEAN };
        checkResult(xrGetActionStateBoolean(m_session, &getInfo, &modifierXAClickState), "Polling a modifier XA state");
        modifierXAs.push_back(modifierXAClickState.currentState);

        getInfo.action = m_inputActions.modifierYB;
        XrActionStateBoolean modifierYBClickState{ XR_TYPE_ACTION_STATE_BOOLEAN };
        checkResult(xrGetActionStateBoolean(m_session, &getInfo, &modifierYBClickState), "Polling a modifier YB state");
        modifierYBs.push_back(modifierYBClickState.currentState);
    }

    for (int handIndex = 0; handIndex < 2; handIndex++) {
        Hand &hand = m_hands[handIndex];
        XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
        getInfo.subactionPath = hand.path;

        bool modifierXA = modifierXAs[handIndex];
        bool modifierYB = modifierYBs[handIndex];

        getInfo.action = m_inputActions.place;
        XrActionStateBoolean thumbstickClickState{ XR_TYPE_ACTION_STATE_BOOLEAN };
        checkResult(xrGetActionStateBoolean(m_session, &getInfo, &thumbstickClickState), "Polling a thumbstick click state");

        getInfo.action = m_inputActions.thumbstickX;
        XrActionStateFloat thumbstickXState{ XR_TYPE_ACTION_STATE_FLOAT };
        checkResult(xrGetActionStateFloat(m_session, &getInfo, &thumbstickXState), "Polling a thumbstick X state");

        getInfo.action = m_inputActions.thumbstickY;
        XrActionStateFloat thumbstickYState{ XR_TYPE_ACTION_STATE_FLOAT };
        checkResult(xrGetActionStateFloat(m_session, &getInfo, &thumbstickYState), "Polling a thumbstick Y state");

        const float radius = sqrt(pow(thumbstickXState.currentState, 2) + pow(thumbstickYState.currentState, 2));

        // PLACE
        if (radius < 0.25 * 1 && thumbstickClickState.changedSinceLastSync && thumbstickClickState.currentState) {
            XrSpaceLocation spaceLocation{ XR_TYPE_SPACE_LOCATION };
            XrResult result = xrLocateSpace(hand.space, m_space, thumbstickClickState.lastChangeTime, &spaceLocation);

            m_cubes.push_back({
                .translation = spaceLocation.pose.position,
                .rotation = spaceLocation.pose.orientation,
                .scale = hand.scale,
                .color = hand.color,
                .type = hand.type
                });
        }

        // The built-in deadzones into the Reverb G2 controllers' thumbsticks make this a bit awkward
        // 1 == max radius
        if (radius >= 0.25 * 1) {
            float angle = acos(thumbstickXState.currentState / radius);
            if (thumbstickYState.currentState < 0) {
                angle = 2 * M_PI - angle;
            }

            if (thumbstickClickState.changedSinceLastSync && thumbstickClickState.currentState) {
                hand.m_colorStartingAngle = angle;
                hand.m_originalColor = hand.color;
            }
            else {
                XrColor4f color;
                // CURRENT COLOR, WHITE AND BLACK
                if (hand.m_colorStartingAngle >= 0) {
                    bool isClockwise;
                    if (hand.m_colorStartingAngle < angle) {
                        isClockwise = false;
                    }
                    else {
                        isClockwise = true;
                    }

                    float angleDelta = abs(angle - hand.m_colorStartingAngle);
                    if (M_PI < angleDelta) {
                        isClockwise = !isClockwise;
                        angleDelta = 2 * M_PI - abs(angle - hand.m_colorStartingAngle);
                    }

                    // Direction with shortest path from the original angle to the current angle
                    if (isClockwise) {
                        // Old color -> black
                        if (angleDelta <= 2. / 3. * M_PI) {
                            float multiplier = (1. - angleDelta / (2. / 3. * M_PI));
                            color.r = hand.m_originalColor.r * multiplier;
                            color.g = hand.m_originalColor.g * multiplier;
                            color.b = hand.m_originalColor.b * multiplier;
                        }
                        // Black -> white
                        else {
                            color.r = 1. * ((angleDelta - 2. / 3. * M_PI) / (2. / 3. * M_PI));
                            color.g = color.r;
                            color.b = color.r;
                        }
                    }
                    else {
                        // Old color -> white
                        if (angleDelta <= 2. / 3. * M_PI) {
                            float multiplier = (angleDelta / (2. / 3. * (float)M_PI));
                            color.r = hand.m_originalColor.r + (1. - hand.m_originalColor.r) * multiplier;
                            color.g = hand.m_originalColor.g + (1. - hand.m_originalColor.g) * multiplier;
                            color.b = hand.m_originalColor.b + (1. - hand.m_originalColor.b) * multiplier;
                        }
                        // White -> black
                        else {
                            color.r = 1. * (1. - (angleDelta - 2. / 3. * M_PI) / (2. / 3. * M_PI));
                            color.g = color.r;
                            color.b = color.r;
                        }
                    }
                }
                // RGB
                // TODO possibly add a second dimension for intensity of the colour
                else {
                    if (11. / 6. * M_PI < angle || angle <= 1. / 2. * M_PI) {
                        color.g = 0;
                        if (11. / 6. * M_PI < angle) {
                            float angleDelta = angle - 11. / 6. * M_PI;
                            color.r = angleDelta / (2. / 3. * M_PI);
                            color.b = 1 - color.r;
                        }
                        else {
                            float angleDelta = 1. / 2. * M_PI - angle;
                            color.b = angleDelta / (2. / 3. * M_PI);
                            color.r = 1 - color.b;
                        }
                    }
                    else if (1. / 2. * M_PI < angle && angle <= 7. / 6. * M_PI) {
                        color.b = 0;
                        float angleDelta = angle - 1. / 2. * M_PI;
                        color.g = angleDelta / (2. / 3. * M_PI);
                        color.r = 1 - color.g;
                    }
                    else {
                        color.r = 0;
                        float angleDelta = angle - 7. / 6. * M_PI;
                        color.b = angleDelta / (2. / 3. * M_PI);
                        color.g = 1 - color.b;
                    }
                }

                hand.color = color;
            }
        }
        // the thumbstick's radius is not large enough so the color picker is not enabled and there's no starting angle for CWB
        else {
            hand.m_colorStartingAngle = -1;
        }

        static const float MAX_CUBE_SCALE = 10.f;
        static const float MIN_CUBE_SCALE = .01f;

        getInfo.action = m_inputActions.expand;
        XrActionStateFloat triggerState{ XR_TYPE_ACTION_STATE_FLOAT };
        checkResult(xrGetActionStateFloat(m_session, &getInfo, &triggerState), "Polling a trigger state");
        if (triggerState.currentState && min(min(hand.scale.x, hand.scale.y), hand.scale.z) < MAX_CUBE_SCALE) {
            float delta = triggerState.currentState * ((MAX_CUBE_SCALE - 1) + 10 * (1 - MIN_CUBE_SCALE)) * 0.001f;

            if (modifierXA && modifierYB) {
                hand.scale.z += delta;
                if (hand.scale.z > MAX_CUBE_SCALE) {
                    hand.scale.z = MAX_CUBE_SCALE;
                }
            }
            else if (modifierXA) {
                hand.scale.x += delta;
                if (hand.scale.x > MAX_CUBE_SCALE) {
                    hand.scale.x = MAX_CUBE_SCALE;
                }
            }
            else if (modifierYB) {
                hand.scale.y += delta;
                if (hand.scale.y > MAX_CUBE_SCALE) {
                    hand.scale.y = MAX_CUBE_SCALE;
                }
            }
            else {
                for (float *e : { &hand.scale.x, &hand.scale.y, &hand.scale.z }) {
                    *e += delta;
                    if (*e > MAX_CUBE_SCALE) {
                        *e = MAX_CUBE_SCALE;
                    }
                }
            }
        }

        getInfo.action = m_inputActions.shrink;
        XrActionStateFloat gripState{ XR_TYPE_ACTION_STATE_FLOAT };
        checkResult(xrGetActionStateFloat(m_session, &getInfo, &gripState), "Polling a grip state");
        if (gripState.currentState && max(max(hand.scale.x, hand.scale.y), hand.scale.z) > MIN_CUBE_SCALE) {

            float delta = gripState.currentState * ((MAX_CUBE_SCALE - 1) + 10 * (1 - MIN_CUBE_SCALE)) * 0.001f;
            if (modifierXA && modifierYB) {
                hand.scale.z -= delta;
                if (hand.scale.z < MIN_CUBE_SCALE) {
                    hand.scale.z = MIN_CUBE_SCALE;
                }
            }
            else if (modifierXA) {
                hand.scale.x -= delta;
                if (hand.scale.x < MIN_CUBE_SCALE) {
                    hand.scale.x = MIN_CUBE_SCALE;
                }
            }
            else if (modifierYB) {
                hand.scale.y -= delta;
                if (hand.scale.y < MIN_CUBE_SCALE) {
                    hand.scale.y = MIN_CUBE_SCALE;
                }
            }
            else {
                for (float *e : { &hand.scale.x, &hand.scale.y, &hand.scale.z }) {
                    *e -= delta;
                    if (*e < MIN_CUBE_SCALE) {
                        *e = MIN_CUBE_SCALE;
                    }
                }
            }
        }


        getInfo.action = m_inputActions.modifierYB;
        XrActionStateBoolean modifierYBClickState{ XR_TYPE_ACTION_STATE_BOOLEAN };
        checkResult(xrGetActionStateBoolean(m_session, &getInfo, &modifierYBClickState), "Polling a modifier YB state");
        if (modifierXAs[(handIndex + 1) % 2] && modifierYBClickState.changedSinceLastSync && modifierYB) {
            hand.type = static_cast<CubeType>((static_cast<int>(hand.type) + 1) % 2);
        }
    }
}

void VRCore::render() {
    XrFrameWaitInfo frameWaitInfo{ XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState frameState{ XR_TYPE_FRAME_STATE };
    checkResult(xrWaitFrame(m_session, &frameWaitInfo, &frameState), "Waiting for a frame");

    XrFrameBeginInfo frameBeginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
    checkResult(xrBeginFrame(m_session, &frameBeginInfo), "Beginning a frame");

    std::vector<XrCompositionLayerBaseHeader *> layers;
    XrCompositionLayerProjection projectionLayer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    std::vector<XrCompositionLayerProjectionView> projectionViews(2, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW });

    // this seems to already be true on XR_SESSION_STATE_SYNCHRONIZED before it even gets to XR_SESSION_STATE_VISIBLE? very weird
    if (frameState.shouldRender) {
        XrViewLocateInfo viewLocateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
        viewLocateInfo.viewConfigurationType = m_viewConfigurationType;
        viewLocateInfo.displayTime = frameState.predictedDisplayTime;
        viewLocateInfo.space = m_space;

        XrViewState viewState{ XR_TYPE_VIEW_STATE };
        uint32_t viewCountOutput;
        checkResult(xrLocateViews(m_session, &viewLocateInfo, &viewState, VIEW_COUNT, &viewCountOutput, m_views.data()), "Locating the views");

        const unsigned int imageWidth = m_configViews[0].recommendedImageRectWidth;
        const unsigned int imageHeight = m_configViews[0].recommendedImageRectHeight;
        for (int i = 0; i < VIEW_COUNT; i++) {
            XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            uint32_t swapchainImageIndex;
            checkResult(xrAcquireSwapchainImage(m_swapchains[i], &acquireInfo, &swapchainImageIndex), "Acquiring a swapchain image");

            XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            waitInfo.timeout = XR_INFINITE_DURATION;
            checkResult(xrWaitSwapchainImage(m_swapchains[i], &waitInfo), "Waiting for a swapchain image");

            projectionViews[i].pose = m_views[i].pose;
            projectionViews[i].fov = m_views[i].fov;
            projectionViews[i].subImage.swapchain = m_swapchains[i];
            projectionViews[i].subImage.imageRect.extent = { (int32_t)imageWidth, (int32_t)imageHeight };

            glBindFramebuffer(GL_FRAMEBUFFER, m_frameBuffer[swapchainImageIndex]);
            glViewport(0, 0, imageWidth, imageHeight);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_images[i][swapchainImageIndex].image, 0);
            glClear(GL_COLOR_BUFFER_BIT);

            XrMatrix4x4f projection;
            XrMatrix4x4f::CreateProjectionFov(&projection, m_views[i].fov, 0.1f, 100.0f);
            XrMatrix4x4f transformation;
            XrVector3f scale{ 1.f, 1.f, 1.f };
            XrMatrix4x4f::CreateTranslationRotationScale(&transformation, &m_views[i].pose.position, &m_views[i].pose.orientation, &scale);
            XrMatrix4x4f viewTransformation;
            XrMatrix4x4f::InvertRigidBody(&viewTransformation, &transformation);
            XrMatrix4x4f viewProjection;
            XrMatrix4x4f::Multiply(&viewProjection, &projection, &viewTransformation);

            for (Hand hand : m_hands) {
                XrSpaceLocation spaceLocation{ XR_TYPE_SPACE_LOCATION };
                checkResult(xrLocateSpace(hand.space, m_space, frameState.predictedDisplayTime, &spaceLocation), "Locating an action space");

                XrMatrix4x4f modelTransformation;
                XrMatrix4x4f::CreateTranslationRotationScale(&modelTransformation, &spaceLocation.pose.position, &spaceLocation.pose.orientation, &hand.scale);

                XrMatrix4x4f modelViewProjection;
                XrMatrix4x4f::Multiply(&modelViewProjection, &viewProjection, &modelTransformation);
                glUniformMatrix4fv(m_modelViewProjectionUniformId, 1, GL_FALSE, modelViewProjection.m);

                std::vector<GLfloat> color{ hand.color.r, hand.color.g, hand.color.b };
                glUniform3fv(m_vertexColorUniformId, 1, color.data());

                drawCube(hand.type);
            }

            for (const Cube &cube : m_cubes) {
                XrMatrix4x4f modelTransformation;
                XrMatrix4x4f::CreateTranslationRotationScale(&modelTransformation, &cube.translation, &cube.rotation, &cube.scale);

                XrMatrix4x4f modelViewProjection;
                XrMatrix4x4f::Multiply(&modelViewProjection, &viewProjection, &modelTransformation);

                glUniformMatrix4fv(m_modelViewProjectionUniformId, 1, GL_FALSE, modelViewProjection.m);

                std::vector<GLfloat> color{ cube.color.r, cube.color.g, cube.color.b };
                glUniform3fv(m_vertexColorUniformId, 1, color.data());

                drawCube(cube.type);
            }

            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            checkResult(xrReleaseSwapchainImage(m_swapchains[i], &releaseInfo), "Releasing a swapchain image");
        }

        projectionLayer.space = m_space;
        projectionLayer.viewCount = (uint32_t)projectionViews.size();
        projectionLayer.views = projectionViews.data();
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&projectionLayer));
    }

    XrFrameEndInfo frameEndInfo{ XR_TYPE_FRAME_END_INFO };
    frameEndInfo.displayTime = frameState.predictedDisplayTime;
    frameEndInfo.environmentBlendMode = m_environmentBlendMode;
    frameEndInfo.layerCount = (uint32_t)layers.size();
    frameEndInfo.layers = layers.data();
    checkResult(xrEndFrame(m_session, &frameEndInfo), "Ending a frame");
}

void VRCore::drawCube(CubeType type) {
    glBindVertexArray(m_vertexArrayId);

    if (type == CubeType::EMPTY) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_emptyCubeIndexBufferId);

        glDrawElements(GL_LINE_LOOP, 4, GL_UNSIGNED_INT, (void *)0);
        glDrawElements(GL_LINE_LOOP, 4, GL_UNSIGNED_INT, (void *)(4 * sizeof(GLuint)));
        glDrawElements(GL_LINES, 8, GL_UNSIGNED_INT, (void *)(8 * sizeof(GLuint)));

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
    else if (type == CubeType::FILLED) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_filledCubeIndexBufferId);

        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, (void *)0);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }

    glBindVertexArray(0);
}

void VRCore::createWindow() {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);

    SDL_Window *window = SDL_CreateWindow("", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 0, 0, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, context);
}

void VRCore::createInstance() {
    if (m_instance != nullptr) {
        throw std::runtime_error("Instance shoudn't be already initialized");
    }

    std::vector<const char *> extensions = getExtensions();

    XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
    createInfo.enabledExtensionCount = (uint32_t)extensions.size();
    createInfo.enabledExtensionNames = extensions.data();

    createInfo.applicationInfo = { "OpenXR test", 0, "", 0, XR_CURRENT_API_VERSION };
    checkResult(xrCreateInstance(&createInfo, &m_instance), "Creating the OXR instance");
}

std::vector<const char *> VRCore::getExtensions() const {
    std::vector<const char *> extensions;
    uint32_t extensionCount;
    checkResult(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr), "Getting the count of available OXR extensions");

    std::vector<XrExtensionProperties> extensionProperties(extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });
    checkResult(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensionProperties.data()), "Getting the properties of available OXR extensions");

    for (uint32_t i = 0; i < extensionCount; i++) {
        spdlog::info("EXT: {}", extensionProperties[i].extensionName);
    }

    std::vector<const char *> extensionNames = {
        "XR_KHR_opengl_enable",
        "XR_EXT_hp_mixed_reality_controller"
    };

    for (const auto &extensionName : extensionNames) {
        for (const auto &extensionProperty : extensionProperties) {
            if (strcmp(extensionProperty.extensionName, extensionName) == 0) {
                extensions.push_back(extensionName);
                break;
            }
        }
    }

    if (extensionNames.size() != extensions.size()) {
        throw std::runtime_error("Not allrequired extensions are supported");
    }

    return extensions;
}

void VRCore::initSystem() {
    if (m_instance == XR_NULL_HANDLE) {
        throw std::runtime_error("Instance not created");
    }
    if (m_systemId != XR_NULL_SYSTEM_ID) {
        throw std::runtime_error("System shouldn't be already initialized");
    }

    XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    checkResult(xrGetSystem(m_instance, &systemInfo, &m_systemId), "Getting the system");

    uint32_t countEBMs;
    checkResult(xrEnumerateEnvironmentBlendModes(m_instance, m_systemId, m_viewConfigurationType, 0, &countEBMs, nullptr), "Enumerating EBMs");

    if (!countEBMs) {
        throw std::runtime_error("At least one EBM must be supported");
    }

    std::vector<XrEnvironmentBlendMode> environmentBlendModes(countEBMs);
    checkResult(xrEnumerateEnvironmentBlendModes(m_instance, m_systemId, m_viewConfigurationType, countEBMs, &countEBMs, environmentBlendModes.data()), "Acquiring EBMs");

    if (std::find(environmentBlendModes.begin(), environmentBlendModes.end(), m_environmentBlendMode) == environmentBlendModes.end()) {
        throw std::runtime_error("HMD must support the opaque blend mode");
    }
}

void VRCore::initSession() {
    if (m_instance == XR_NULL_HANDLE) {
        throw std::runtime_error("Instance not created");
    }
    if (m_systemId == XR_NULL_SYSTEM_ID) {
        throw std::runtime_error("System not initialized");
    }
    if (m_session != XR_NULL_HANDLE) {
        throw std::runtime_error("Session shoudn't be already initialized");
    }

    PFN_xrGetOpenGLGraphicsRequirementsKHR pfnGetOpenGLGraphicsRequirementsKHR;
    checkResult(xrGetInstanceProcAddr(m_instance, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&pfnGetOpenGLGraphicsRequirementsKHR), "Obtaining OpenGL EXT function address");

    XrGraphicsRequirementsOpenGLKHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
    checkResult(pfnGetOpenGLGraphicsRequirementsKHR(m_instance, m_systemId, &graphicsRequirements), "Getting graphics requirements");

    XrGraphicsBindingOpenGLWin32KHR graphicsBinding{
        XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR,
        nullptr,
        wglGetCurrentDC(),
        wglGetCurrentContext()
    };

    XrSessionCreateInfo createInfo{ XR_TYPE_SESSION_CREATE_INFO };
    createInfo.next = &graphicsBinding;
    createInfo.systemId = m_systemId;
    checkResult(xrCreateSession(m_instance, &createInfo, &m_session), "Creating the session");
}

void VRCore::initReferenceSpace() {
    if (m_session == XR_NULL_HANDLE) {
        throw std::runtime_error("Session not initialized");
    }

    XrReferenceSpaceCreateInfo createInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    XrReferenceSpaceType spaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    createInfo.referenceSpaceType = spaceType;

    XrPosef defaultPose{
        .orientation = { 0, 0, 0, 1 },
        .position = { 0, 0, 0 }
    };
    createInfo.poseInReferenceSpace = defaultPose;

    checkResult(xrCreateReferenceSpace(m_session, &createInfo, &m_space), "Creating the reference space");

    XrExtent2Df bounds;
    xrGetReferenceSpaceBoundsRect(m_session, spaceType, &bounds);
    spdlog::info("BOUNDS SIZE: {}x{}", bounds.width, bounds.height);
}

void VRCore::initActions() {
    if (m_session == XR_NULL_HANDLE) {
        throw std::runtime_error("Session not initialized");
    }

    xrStringToPath(m_instance, "/user/hand/left", &m_hands[0].path);
    xrStringToPath(m_instance, "/user/hand/right", &m_hands[1].path);
    std::vector<XrPath> m_handPaths = { m_hands[0].path, m_hands[1].path };

    XrActionSetCreateInfo actionSetInfo{ XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy_s(actionSetInfo.actionSetName, "interaction");
    strcpy_s(actionSetInfo.localizedActionSetName, "Interaction");
    actionSetInfo.priority = 0;
    checkResult(xrCreateActionSet(m_instance, &actionSetInfo, &m_actionSet), "Creating the action set");

    XrActionCreateInfo actionInfo{ XR_TYPE_ACTION_CREATE_INFO };
    actionInfo.countSubactionPaths = uint32_t(m_handPaths.size());
    actionInfo.subactionPaths = m_handPaths.data();

    strcpy_s(actionInfo.actionName, "pose");
    strcpy_s(actionInfo.localizedActionName, "Pose");
    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    checkResult(xrCreateAction(m_actionSet, &actionInfo, &m_inputActions.pose), "Creating action \"Pose\"");

    strcpy_s(actionInfo.actionName, "place");
    strcpy_s(actionInfo.localizedActionName, "Place");
    actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    checkResult(xrCreateAction(m_actionSet, &actionInfo, &m_inputActions.place), "Creating action \"Place\"");

    strcpy_s(actionInfo.actionName, "expand");
    strcpy_s(actionInfo.localizedActionName, "Expand");
    actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    checkResult(xrCreateAction(m_actionSet, &actionInfo, &m_inputActions.expand), "Creating action \"Expand\"");

    strcpy_s(actionInfo.actionName, "shrink");
    strcpy_s(actionInfo.localizedActionName, "Shrink");
    actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    checkResult(xrCreateAction(m_actionSet, &actionInfo, &m_inputActions.shrink), "Creating action \"Shrink\"");

    strcpy_s(actionInfo.actionName, "modifier_xa");
    strcpy_s(actionInfo.localizedActionName, "Modifier XA");
    actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    checkResult(xrCreateAction(m_actionSet, &actionInfo, &m_inputActions.modifierXA), "Creating action \"Modifier XA\"");

    strcpy_s(actionInfo.actionName, "modifier_yb");
    strcpy_s(actionInfo.localizedActionName, "Modifier YB");
    actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    checkResult(xrCreateAction(m_actionSet, &actionInfo, &m_inputActions.modifierYB), "Creating action \"Modifier YB\"");

    strcpy_s(actionInfo.actionName, "thumbstick_x");
    strcpy_s(actionInfo.localizedActionName, "Thumbstick X");
    actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    checkResult(xrCreateAction(m_actionSet, &actionInfo, &m_inputActions.thumbstickX), "Creating action \"Thumbstick X\"");

    strcpy_s(actionInfo.actionName, "thumbstick_y");
    strcpy_s(actionInfo.localizedActionName, "Thumbstick Y");
    actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    checkResult(xrCreateAction(m_actionSet, &actionInfo, &m_inputActions.thumbstickY), "Creating action \"Thumbstick Y\"");

    // Haptics are mostly an annoyance
    //strcpy_s(actionInfo.actionName, "vibrate_left");
    //strcpy_s(actionInfo.localizedActionName, "Vibrate Left");
    //actionInfo.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
    //checkResult(xrCreateAction(m_actionSet, &actionInfo, &m_hands[0].vibrateAction), "Creating action \"Vibrate Left\"");
    //
    //strcpy_s(actionInfo.actionName, "vibrate_right");
    //strcpy_s(actionInfo.localizedActionName, "Vibrate Right");
    //actionInfo.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
    //checkResult(xrCreateAction(m_actionSet, &actionInfo, &m_hands[1].vibrateAction), "Creating action \"Vibrate Right\"");


    std::vector<std::pair<XrAction, const char *>> actionPairs{
        {m_inputActions.pose, "/input/grip/pose"},
        {m_inputActions.place, "/input/thumbstick/click"},
        {m_inputActions.expand, "/input/trigger/value"},
        {m_inputActions.shrink, "/input/squeeze/value"},
        {m_inputActions.thumbstickX, "/input/thumbstick/x"},
        {m_inputActions.thumbstickY, "/input/thumbstick/y"},
        // This button is being hijacked by the SteamVR implementation of the OpenXR runtime: "Invalid input type click for controller"
        //{m_inputActions.changeType, "/input/menu/click"}
    };

    std::vector<XrActionSuggestedBinding> actionBindings;
    actionBindings.reserve(2 * actionPairs.size());

    XrActionSuggestedBinding actionBinding;
    for (const auto &actionPair : actionPairs) {
        actionBinding.action = actionPair.first;
        for (const std::string &side : { "/left", "/right" }) {
            std::string path = "/user/hand" + side + actionPair.second;
            checkResult(xrStringToPath(m_instance, path.c_str(), &actionBinding.binding), "String to path: " + path);
            actionBindings.push_back(actionBinding);
        }
    }

    actionBinding.action = m_inputActions.modifierXA;
    xrStringToPath(m_instance, "/user/hand/left/input/x/click", &actionBinding.binding);
    actionBindings.push_back(actionBinding);
    actionBinding.action = m_inputActions.modifierXA;
    xrStringToPath(m_instance, "/user/hand/right/input/a/click", &actionBinding.binding);
    actionBindings.push_back(actionBinding);

    actionBinding.action = m_inputActions.modifierYB;
    xrStringToPath(m_instance, "/user/hand/left/input/y/click", &actionBinding.binding);
    actionBindings.push_back(actionBinding);
    actionBinding.action = m_inputActions.modifierYB;
    xrStringToPath(m_instance, "/user/hand/right/input/b/click", &actionBinding.binding);
    actionBindings.push_back(actionBinding);

    //actionBinding.action = m_hands[0].vibrateAction;
    //xrStringToPath(m_instance, "/user/hand/left/output/haptic", &actionBinding.binding);
    //actionBindings.push_back(actionBinding);
    //actionBinding.action = m_hands[1].vibrateAction;
    //xrStringToPath(m_instance, "/user/hand/right/output/haptic", &actionBinding.binding);
    //actionBindings.push_back(actionBinding);

    XrInteractionProfileSuggestedBinding profileBindings{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    xrStringToPath(m_instance, "/interaction_profiles/hp/mixed_reality_controller", &profileBindings.interactionProfile);
    profileBindings.countSuggestedBindings = (uint32_t)actionBindings.size();
    profileBindings.suggestedBindings = actionBindings.data();
    checkResult(xrSuggestInteractionProfileBindings(m_instance, &profileBindings), "Suggesting interaction bindings");

    XrSessionActionSetsAttachInfo attachInfo{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &m_actionSet;
    checkResult(xrAttachSessionActionSets(m_session, &attachInfo), "Attaching action sets to the session");

    XrActionSpaceCreateInfo actionSpaceInfo{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
    actionSpaceInfo.action = m_inputActions.pose;
    actionSpaceInfo.subactionPath = m_handPaths[0];
    checkResult(xrCreateActionSpace(m_session, &actionSpaceInfo, &m_hands[0].space), "Creating an action space");
    actionSpaceInfo.subactionPath = m_handPaths[1];
    checkResult(xrCreateActionSpace(m_session, &actionSpaceInfo, &m_hands[1].space), "Creating an action space");
}

void VRCore::initRendering() {
    if (m_session == XR_NULL_HANDLE) {
        throw std::runtime_error("Session not initialized");
    }

    uint32_t viewCount;
    checkResult(xrEnumerateViewConfigurationViews(m_instance, m_systemId, m_viewConfigurationType, 0, &viewCount, nullptr), "Enumerating view configuration views");
    if (viewCount != VIEW_COUNT) {
        throw std::runtime_error("Wrong number of views\t" + viewCount);
    }

    m_configViews.resize(VIEW_COUNT, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    m_views.resize(VIEW_COUNT, { XR_TYPE_VIEW });

    checkResult(xrEnumerateViewConfigurationViews(m_instance, m_systemId, m_viewConfigurationType, VIEW_COUNT, &viewCount, m_configViews.data()), "Acquiring configuration views");

    XrSwapchainCreateInfo swapchainInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t swapchainFormatCount;
    checkResult(xrEnumerateSwapchainFormats(m_session, 0, &swapchainFormatCount, nullptr), "Enumerating swapchain formats");

    std::vector<int64_t> swapchainFormats(swapchainFormatCount);
    checkResult(xrEnumerateSwapchainFormats(m_session, (uint32_t)swapchainFormats.size(), &swapchainFormatCount, swapchainFormats.data()), "Acquiring swapchain formats");
    swapchainInfo.format = swapchainFormats[0];

    const auto &view = m_configViews[0];
    swapchainInfo.sampleCount = view.recommendedSwapchainSampleCount;
    swapchainInfo.width = view.recommendedImageRectWidth;
    swapchainInfo.height = view.recommendedImageRectHeight;
    swapchainInfo.faceCount = 1;
    swapchainInfo.mipCount = 1;
    swapchainInfo.arraySize = 1;

    m_swapchains.resize(VIEW_COUNT);
    m_images.resize(VIEW_COUNT);
    for (uint32_t i = 0; i < VIEW_COUNT; i++) {
        checkResult(xrCreateSwapchain(m_session, &swapchainInfo, &m_swapchains[i]), "Creating a swapchain");

        checkResult(xrEnumerateSwapchainImages(m_swapchains[i], 0, &m_swapchainLength, nullptr), "Acquiring a swapchain length");
        m_images[i].resize(m_swapchainLength, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR });

        checkResult(xrEnumerateSwapchainImages(m_swapchains[i], m_swapchainLength, &m_swapchainLength, reinterpret_cast<XrSwapchainImageBaseHeader *>(m_images[i].data())), "Filling swapchain images");
    }
}

void VRCore::initGL() {
    m_frameBuffer.resize(m_swapchainLength);
    glGenFramebuffers(m_swapchainLength, m_frameBuffer.data());


    m_programId = glCreateProgram();

    GLuint vertexShaderId = glCreateShader(GL_VERTEX_SHADER);
    GLuint fragmentShaderId = glCreateShader(GL_FRAGMENT_SHADER);

    static const GLchar *vertexShader = R"(
        #version 330 core
        in vec3 position;
        out vec3 fragmentColor;
        uniform mat4 u_modelViewProjection;
        uniform vec3 u_vertexColor;

        void main() {
            fragmentColor = u_vertexColor;
            gl_Position = u_modelViewProjection * vec4(position, 1);
        }
    )";

    static const GLchar *fragmentShader = R"(
        #version 330 core
        in vec3 fragmentColor;
        out vec3 color;

        void main() {
            color = fragmentColor;
        }
    )";

    auto checkShader = [](GLuint shaderId, std::string description) {
        GLint result;
        glGetShaderiv(shaderId, GL_COMPILE_STATUS, &result);
        if (result == GL_FALSE) {
            GLint infoLogLength;
            glGetShaderiv(shaderId, GL_INFO_LOG_LENGTH, &infoLogLength);

            std::vector<GLchar> infoLog(infoLogLength);
            glGetShaderInfoLog(shaderId, infoLogLength, nullptr, infoLog.data());
            throw std::runtime_error(description + "\t" + infoLog.data());
        }
    };

    auto checkProgram = [](GLuint programId, std::string description) {
        GLint result;
        glGetProgramiv(programId, GL_LINK_STATUS, &result);
        if (result == GL_FALSE) {
            GLint infoLogLength;
            glGetProgramiv(programId, GL_INFO_LOG_LENGTH, &infoLogLength);

            std::vector<GLchar> infoLog(infoLogLength);
            glGetProgramInfoLog(programId, infoLogLength, nullptr, infoLog.data());

            throw std::runtime_error(description + "\t" + infoLog.data());
        }
    };

    glShaderSource(vertexShaderId, 1, &vertexShader, NULL);
    glCompileShader(vertexShaderId);
    checkShader(vertexShaderId, "Checking the vertex shader");
    glAttachShader(m_programId, vertexShaderId);

    glShaderSource(fragmentShaderId, 1, &fragmentShader, NULL);
    glCompileShader(fragmentShaderId);
    checkShader(fragmentShaderId, "Checking the fragment shader");
    glAttachShader(m_programId, fragmentShaderId);

    glLinkProgram(m_programId);
    checkProgram(m_programId, "Checking the program linkage");

    glDeleteShader(vertexShaderId);
    glDeleteShader(fragmentShaderId);

    m_modelViewProjectionUniformId = glGetUniformLocation(m_programId, "u_modelViewProjection");
    m_vertexColorUniformId = glGetUniformLocation(m_programId, "u_vertexColor");



    static const std::vector<GLfloat> cubeVertexBufferData = {
        0.1, -0.1, 0.1,
        -0.1, -0.1, 0.1,
        -0.1, -0.1, -0.1,
        0.1, -0.1, -0.1,

        0.1, 0.1, 0.1,
        -0.1, 0.1, 0.1,
        -0.1, 0.1, -0.1,
        0.1, 0.1, -0.1,
    };

    // not that many indices so it should be fine
    static const std::vector<GLuint> emptyCubeIndexBufferData = {
        0, 1, 2, 3,

        4, 5, 6, 7,

        0, 4,

        1, 5,

        2, 6,

        3, 7
    };

    static const std::vector<GLuint> filledCubeIndexBufferData = {
        0, 2, 1,
        0, 3, 2,

        4, 6, 5,
        4, 7, 6,

        0, 5, 1,
        0, 4, 5,

        3, 6, 2,
        3, 7, 6,

        3, 4, 0,
        3, 7, 4,

        2, 5, 1,
        2, 6, 5
    };


    glGenVertexArrays(1, &m_vertexArrayId);
    glBindVertexArray(m_vertexArrayId);

    glGenBuffers(1, &m_vertexBufferId);
    glBindBuffer(GL_ARRAY_BUFFER, m_vertexBufferId);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cubeVertexBufferData[0]) * cubeVertexBufferData.size(), &cubeVertexBufferData[0], GL_STATIC_DRAW);

    glGenBuffers(1, &m_emptyCubeIndexBufferId);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_emptyCubeIndexBufferId);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(emptyCubeIndexBufferData[0]) * emptyCubeIndexBufferData.size(), &emptyCubeIndexBufferData[0], GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    glGenBuffers(1, &m_filledCubeIndexBufferId);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_filledCubeIndexBufferId);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(filledCubeIndexBufferData[0]) * filledCubeIndexBufferData.size(), &filledCubeIndexBufferData[0], GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (GLvoid *)0);
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glBindVertexArray(0);

    glUseProgram(m_programId);
}

XrResult VRCore::checkResult(const XrResult result, const std::string description) const {
    if (result != XR_SUCCESS) {
        if (m_instance != nullptr) {
            char resultBuffer[XR_MAX_RESULT_STRING_SIZE];
            xrResultToString(m_instance, result, resultBuffer);
            throw std::runtime_error(description + "\t" + resultBuffer);
        }
        else {
            throw std::runtime_error(description);
        }
    }

    return result;
}

VRCore::~VRCore() {
    glDeleteBuffers(1, &m_vertexBufferId);
    glDeleteBuffers(1, &m_emptyCubeIndexBufferId);
    glDeleteBuffers(1, &m_filledCubeIndexBufferId);
    glDeleteVertexArrays(1, &m_vertexArrayId);
    glDeleteProgram(m_programId);

    for (auto &swapchain : m_swapchains) {
        xrDestroySwapchain(swapchain);
    }

    if (m_actionSet != XR_NULL_HANDLE) {
        for (auto &hand : m_hands) {
            xrDestroySpace(hand.space);
        }
        xrDestroyActionSet(m_actionSet);
    }

    if (m_space != XR_NULL_HANDLE) {
        xrDestroySpace(m_space);
    }

    if (m_session != XR_NULL_HANDLE) {
        xrDestroySession(m_session);
    }

    if (m_instance != XR_NULL_HANDLE) {
        xrDestroyInstance(m_instance);
    }
}
