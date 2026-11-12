#import <Cocoa/Cocoa.h>
#include "include/cef_application_mac.h"

@interface WBApplication : NSApplication<CefAppProtocol> {
    BOOL handlingSendEvent;
}
@end
@implementation WBApplication
- (BOOL)isHandlingSendEvent { return handlingSendEvent; }
- (void)setHandlingSendEvent:(BOOL)value { handlingSendEvent = value; }
- (void)sendEvent:(NSEvent*)event {
    CefScopedSendingEvent sendingEvent;
    [super sendEvent:event];
}
@end
void wb_prepare_application() { [WBApplication sharedApplication]; }
