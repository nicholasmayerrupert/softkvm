#import "CGVirtualDisplayPrivate.h"

// Private CoreGraphics interfaces (same declarations DeskPad / BetterDisplay rely on).
// We only send messages to instances obtained via NSClassFromString, so no private
// class symbols are referenced at link time.

@interface CGVirtualDisplayDescriptor : NSObject
@property (retain, nonatomic) dispatch_queue_t queue;
@property (retain, nonatomic) NSString *name;
@property (nonatomic) unsigned int maxPixelsHigh;
@property (nonatomic) unsigned int maxPixelsWide;
@property (nonatomic) CGSize sizeInMillimeters;
@property (nonatomic) unsigned int serialNum;
@property (nonatomic) unsigned int productID;
@property (nonatomic) unsigned int vendorID;
@property (copy, nonatomic) void (^terminationHandler)(id, id);
@end

@interface CGVirtualDisplayMode : NSObject
- (instancetype)initWithWidth:(unsigned int)width height:(unsigned int)height refreshRate:(double)refreshRate;
@end

@interface CGVirtualDisplaySettings : NSObject
@property (retain, nonatomic) NSArray *modes;
@property (nonatomic) unsigned int hiDPI;
@end

@interface CGVirtualDisplay : NSObject
@property (readonly, nonatomic) CGDirectDisplayID displayID;
- (instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor *)descriptor;
- (BOOL)applySettings:(CGVirtualDisplaySettings *)settings;
@end

id SKCreateVirtualDisplay(NSString *name,
                          unsigned int width,
                          unsigned int height,
                          double refreshRate,
                          unsigned int serial,
                          CGDirectDisplayID *outDisplayID) {
    Class descClass = NSClassFromString(@"CGVirtualDisplayDescriptor");
    Class displayClass = NSClassFromString(@"CGVirtualDisplay");
    Class modeClass = NSClassFromString(@"CGVirtualDisplayMode");
    Class settingsClass = NSClassFromString(@"CGVirtualDisplaySettings");
    if (!descClass || !displayClass || !modeClass || !settingsClass) {
        NSLog(@"SoftKVM: CGVirtualDisplay private API not available on this macOS");
        return nil;
    }

    CGVirtualDisplayDescriptor *desc = [[descClass alloc] init];
    desc.queue = dispatch_get_main_queue();
    desc.name = name;
    desc.maxPixelsWide = width;
    desc.maxPixelsHigh = height;
    // Assume ~110 ppi (typical 27" 1440p desktop monitor) for the physical size.
    const double mmPerPixel = 25.4 / 110.0;
    desc.sizeInMillimeters = CGSizeMake(width * mmPerPixel, height * mmPerPixel);
    desc.vendorID = 0x5346;   // arbitrary, stable
    desc.productID = 0x4b56;  // arbitrary, stable
    desc.serialNum = serial;
    desc.terminationHandler = ^(id a, id b) {
        NSLog(@"SoftKVM: virtual display %@ terminated", name);
    };

    CGVirtualDisplay *display = [[displayClass alloc] initWithDescriptor:desc];
    if (!display) {
        return nil;
    }

    CGVirtualDisplaySettings *settings = [[settingsClass alloc] init];
    settings.hiDPI = 0;
    settings.modes = @[ [[modeClass alloc] initWithWidth:width height:height refreshRate:refreshRate] ];
    if (![display applySettings:settings]) {
        NSLog(@"SoftKVM: applySettings failed for %ux%u@%.0f", width, height, refreshRate);
        return nil;
    }

    if (outDisplayID) {
        *outDisplayID = display.displayID;
    }
    return display;
}
