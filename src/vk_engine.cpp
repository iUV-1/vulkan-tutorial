//> includes
#include "vk_engine.h"
#include "VkBootstrap.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <vk_initializers.h>
#include <vk_types.h>

#include <chrono>
#include <thread>

VulkanEngine* loadedEngine = nullptr;

VulkanEngine& VulkanEngine::Get() { return *loadedEngine; }
void VulkanEngine::init()
{
    // only one engine initialization is allowed with the application.
    assert(loadedEngine == nullptr);
    loadedEngine = this;

    // We initialize SDL and create a window with it.
    SDL_Init(SDL_INIT_VIDEO);

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN);

    _window = SDL_CreateWindow(
        "Vulkan Engine",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        _windowExtent.width,
        _windowExtent.height,
        window_flags);

    // everything went fine
    _isInitialized = true;
}

void VulkanEngine::cleanup()
{
    if (_isInitialized) {

        vkDeviceWaitIdle(_device);

        for (int i = 0; i < FRAME_OVERLAP; i++) {
            // destroying the pools will also destroy the buffers
            vkDestroyCommandPool(_device, _frames[i]._commandPool, nullptr);
        }

        destroy_swapchain();

        vkDestroySurfaceKHR(_instance, _surface, nullptr);
        vkDestroyDevice(_device, nullptr);
        vkDestroyInstance(_instance, nullptr);

        vkb::destroy_debug_utils_messenger(_instance, _debug_messenger);
        SDL_DestroyWindow(_window);
    }

    // clear engine pointer
    loadedEngine = nullptr;
}

void VulkanEngine::draw()
{
    // nothing yet
}

void VulkanEngine::run()
{
    SDL_Event e;
    bool bQuit = false;

    // main loop
    while (!bQuit) {
        // Handle events on queue
        while (SDL_PollEvent(&e) != 0) {
            // close the window when user alt-f4s or clicks the X button
            if (e.type == SDL_QUIT)
                bQuit = true;

            if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                    stop_rendering = true;
                }
                if (e.window.event == SDL_WINDOWEVENT_RESTORED) {
                    stop_rendering = false;
                }
            }

            if(e.type == SDL_KEYDOWN) {
                SDL_Keycode key = e.key.keysym.sym;
                fmt::println("key down: {} {}", SDL_GetKeyName(key), key);
            }
        }

        // do not draw if we are minimized
        if (stop_rendering) {
            // throttle the speed to avoid the endless spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        draw();
    }
}

void VulkanEngine::init_vulkan()
{
    // From the VkBootstrap lib
    // Abstracts the creation of VkInstance
    vkb::InstanceBuilder builder;

    // Make the vulkan instance
    auto inst_ret = builder.set_app_name("Example Vulkan App")
            .request_validation_layers(true)
            .use_default_debug_messenger()
            .require_api_version(1, 3, 0)
            .build();

    vkb::Instance vkb_instance = inst_ret.value();

    _instance = vkb_instance.instance;
    _debug_messenger = vkb_instance.debug_messenger;

    // Choose device and select features
    SDL_Vulkan_CreateSurface(_window, _instance, &_surface);

    //vulkan1.3 features
    VkPhysicalDeviceVulkan13Features features { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features.dynamicRendering = true;
    features.synchronization2 = true;

    VkPhysicalDeviceVulkan12Features features12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    features12.bufferDeviceAddress = true;
    features12.descriptorIndexing = true;

    // use vkbootstrap to select a gpu
    // Select a GPU that can rwite to the SDL surface and support vk1.3 with the
    // correct features
    vkb::PhysicalDeviceSelector selector { vkb_instance };
    vkb::PhysicalDevice physicalDevice = selector
            .set_minimum_version(1, 3)
            .set_required_features_13(features)
            .set_required_features_12(features12)
            .set_surface(_surface)
            .select()
            .value();

    // create the final vulkan device
    vkb::DeviceBuilder deviceBuilder {physicalDevice};
    vkb::Device vkbDevice = deviceBuilder.build().value();

    // Get the VkDevice handle used in the rest of a vulkan application
    _device = vkbDevice.device;
    _chosenGPU = physicalDevice.physical_device;

    // Get a graphics queue with vkbootstrap
    _graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
    _graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();
}
void VulkanEngine::init_swapchain()
{
    create_swapchain(_windowExtent.width, _windowExtent.height);
}

void VulkanEngine::create_swapchain(uint32_t width, uint32_t height) {
    vkb::SwapchainBuilder swapchainBuilder {_chosenGPU, _device, _surface};

    _swapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM; // UNORM is unsigned normal, values are normalized to [0.0, 1.0]

    vkb::Swapchain vkbSwapchain = swapchainBuilder
            .set_desired_format(VkSurfaceFormatKHR{ .format = _swapchainImageFormat, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
            .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR) // Hard VSync
            .set_desired_extent(width, height)
            .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
            .build()
            .value();

    _swapchainExtent = vkbSwapchain.extent;
    // Store swapchain and its related images
    _swapchain = vkbSwapchain.swapchain;
    _swapchainImages = vkbSwapchain.get_images().value();
    _swapchainImageViews = vkbSwapchain.get_image_views().value();
}

void VulkanEngine::destroy_swapchain() {
    vkDestroySwapchainKHR(_device, _swapchain, nullptr);

    for(auto swapchainView: _swapchainImageViews) {
        vkDestroyImageView(_device, swapchainView, nullptr);
    }
}

void VulkanEngine::init_commands()
{
    // Not using vkinit
    /*
    // Create a command pool for commands to be submitted to the graphics queue
    // We also want the pool to allow for resetting of individual command buffers
    VkCommandPoolCreateInfo commandPoolInfo = {};
    commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolInfo.pNext = nullptr;
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolInfo.queueFamilyIndex = _graphicsQueueFamily;

    // vkCreateCommandPool will write to pCommandBuffer if it succeeds
    for(int i = 0; i < FRAME_OVERLAP; i++) {
        VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_frames[i]._commandPool));

        // allocate the default command buffer that we will use for rendering
        VkCommandBufferAllocateInfo cmdAllocInfo = {}; // Set everything in that struct to be 0
        cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAllocInfo.pNext = nullptr;
        cmdAllocInfo.commandPool = _frames[i]._commandPool;
        cmdAllocInfo.commandBufferCount = 1; // Create only 1 buffer from a pool
        cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;

        VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_frames[i]._mainCommandBuffer));
    }
     */

    VkCommandPoolCreateInfo commandPoolInfo = vkinit::command_pool_create_info(_graphicsQueueFamily, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

    for(int i = 0; i < FRAME_OVERLAP; i++) {
        VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_frames[i]._commandPool));

        // allocate the default command buffer that we will use for rendering
        VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(_frames[i]._commandPool, 1);

        VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_frames[i]._mainCommandBuffer));
    }
}
void VulkanEngine::init_sync_structures()
{
    //nothing yet
}