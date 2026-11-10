#include <vulkan/vulkan.h>

#include "MacOSSurface.h"
#include <vulkan/vulkan_raii.hpp>

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>

// ---------------------------------------------------------------------------
// A thin NSView subclass whose backing layer is always a CAMetalLayer.
// Inserted as a subview of the QNSView so Qt's layer remains untouched.
// ---------------------------------------------------------------------------
@interface NeurusMetalView : NSView
@property (nonatomic, strong) CAMetalLayer* metalLayer;
@end

@implementation NeurusMetalView

+ (Class)layerClass {
    return [CAMetalLayer class];
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.wantsLayer = YES;
        _metalLayer = [CAMetalLayer layer];
        _metalLayer.opaque = YES;
        self.layer = _metalLayer;
        self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    }
    return self;
}

- (BOOL)isOpaque {
    return YES;
}

@end

// Association key to attach the NeurusMetalView to the parent QNSView.
static const char kMetalViewKey = 0;

namespace {

/// Configure the native view for Metal rendering by attaching a NeurusMetalView.
/// Returns the CAMetalLayer pointer (as void*) for VkMetalSurfaceCreateInfoEXT,
/// or nullptr on failure.
void* makeViewMetalCompatible(void* nativeHandle)
{
    @autoreleasepool {
        NSView* view = (__bridge NSView*)nativeHandle;
        if (!view || ![view isKindOfClass:[NSView class]])
            return nullptr;

        // Check if we already attached a NeurusMetalView
        NeurusMetalView* metalView = objc_getAssociatedObject(view, &kMetalViewKey);
        if (metalView && [metalView isKindOfClass:[NeurusMetalView class]]) {
            // Update scale in case window moved to another display
            CGFloat scale = view.window ? view.window.backingScaleFactor
                                        : [[NSScreen mainScreen] backingScaleFactor];
            metalView.metalLayer.contentsScale = scale;
            return (__bridge void*)metalView.metalLayer;
        }

        // Create a new NeurusMetalView that fills the parent QNSView
        metalView = [[NeurusMetalView alloc] initWithFrame:view.bounds];
        [view addSubview:metalView];

        // Configure the Metal layer
        CAMetalLayer* layer = metalView.metalLayer;
        CGFloat scale = view.window ? view.window.backingScaleFactor
                                    : [[NSScreen mainScreen] backingScaleFactor];
        layer.contentsScale = scale;

        CGSize drawableSize = view.bounds.size;
        drawableSize.width *= scale;
        drawableSize.height *= scale;
        layer.drawableSize = drawableSize;

        // Associate so we can retrieve it later
        objc_setAssociatedObject(view, &kMetalViewKey, metalView,
                                 OBJC_ASSOCIATION_RETAIN_NONATOMIC);

        return (__bridge void*)layer;
    }
}

/// Update the CAMetalLayer drawable size to match the parent view's current bounds.
void updateMetalLayerSize(void* nativeHandle)
{
    @autoreleasepool {
        NSView* view = (__bridge NSView*)nativeHandle;
        if (!view || ![view isKindOfClass:[NSView class]])
            return;

        NeurusMetalView* metalView = objc_getAssociatedObject(view, &kMetalViewKey);
        if (!metalView)
            return;

        // Resize the subview to match its parent
        metalView.frame = view.bounds;

        CAMetalLayer* layer = metalView.metalLayer;
        CGFloat scale = view.window ? view.window.backingScaleFactor
                                    : [[NSScreen mainScreen] backingScaleFactor];
        layer.contentsScale = scale;

        CGSize drawableSize = view.bounds.size;
        drawableSize.width *= scale;
        drawableSize.height *= scale;
        layer.drawableSize = drawableSize;
    }
}

} // anonymous namespace

namespace neurus {

std::vector<const char*> MacOSSurface::requiredInstanceExtensions() const
{
    return {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_EXT_METAL_SURFACE_EXTENSION_NAME,
        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
    };
}

vk::InstanceCreateFlags MacOSSurface::instanceCreateFlags() const
{
    return vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
}

vk::raii::SurfaceKHR MacOSSurface::createSurface(
    const vk::raii::Instance& instance,
    NativeWindowHandle windowHandle) const
{
    CAMetalLayer* metalLayer = (__bridge CAMetalLayer*)makeViewMetalCompatible(windowHandle);
    vk::MetalSurfaceCreateInfoEXT createInfo({}, metalLayer);
    return vk::raii::SurfaceKHR(instance, createInfo);
}

void MacOSSurface::onResize(NativeWindowHandle windowHandle,
                            uint32_t /*width*/, uint32_t /*height*/) const
{
    updateMetalLayerSize(windowHandle);
}

std::unique_ptr<PlatformSurface> CreatePlatformSurface()
{
    return std::make_unique<MacOSSurface>();
}

} // namespace neurus
