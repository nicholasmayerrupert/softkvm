#pragma once

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

NS_ASSUME_NONNULL_BEGIN

/// Creates a virtual display using Apple's private CGVirtualDisplay API.
///
/// The display exists for as long as the returned object is retained; release it
/// to remove the display. Returns nil if the private API is unavailable or the
/// display could not be created.
///
/// @param name        Name shown in System Settings > Displays.
/// @param width       Width in pixels.
/// @param height      Height in pixels.
/// @param refreshRate Refresh rate in Hz.
/// @param serial      Distinct serial per display so macOS remembers each one's arrangement.
/// @param outDisplayID Receives the CGDirectDisplayID of the new display.
id _Nullable SKCreateVirtualDisplay(NSString *name,
                                    unsigned int width,
                                    unsigned int height,
                                    double refreshRate,
                                    unsigned int serial,
                                    CGDirectDisplayID *outDisplayID);

NS_ASSUME_NONNULL_END
