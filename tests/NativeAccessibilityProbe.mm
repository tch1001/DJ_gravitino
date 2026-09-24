// Exercise Cocoa row retirement at a real autorelease-pool boundary: Qt-only
// accessibility queries do not reproduce native readers retiring cached cells.
#include <QAccessible>
#import <AppKit/AppKit.h>
#import <objc/runtime.h>

@interface NSObject (GravitinoAccessibilityProbe)
+ (id)elementWithId:(QAccessible::Id)identifier;
- (void)updateTableModel;
@end

void retireNativeTableRows(QAccessible::Id tableId)
{
    @autoreleasepool {
        Class cls = objc_lookUpClass("QMacAccessibilityElement");
        id table = [cls elementWithId:tableId];
        // Create the tested rows inside this pool as well, so both their
        // construction autorelease and retirement autorelease drain here.
        [table updateTableModel];
        NSArray* rows = [table accessibilityRows];
        for (id row in rows) {
            for (id cell in [row accessibilityChildren]) {
                // Resolve synthesized placeholders into real, cached cells.
                (void)[cell accessibilityValue];
            }
        }
        [table updateTableModel];
    } // Cocoa retires the old rows here, before the next model notification.
}
