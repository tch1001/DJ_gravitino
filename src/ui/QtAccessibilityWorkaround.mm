// Qt 6.11.0/1's Cocoa bridge deletes the owning table's accessibility interface
// when retiring a synthesized row/column/placeholder cell. Those elements borrow
// the table's ID; only real elements own an ID. Keep this compatibility repair
// local to this process, preserving Qt's table semantics and accessibility.
#include "QtAccessibilityWorkaround.h"

#include <QGuiApplication>
#include <QVersionNumber>
#include <QDebug>

#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include <cstring>

namespace {
Ivar synthesizedRoleIvar = nullptr;
ptrdiff_t accessibleIdOffset = 0;
using Dealloc = void (*)(id, SEL);
using RemoveElements = void (*)(id, SEL, NSArray*);
Dealloc originalDealloc = nullptr;
RemoveElements originalRemoveElements = nullptr;

bool borrowsTableId(id element)
{
    return object_getIvar(element, synthesizedRoleIvar) != nil;
}

void deallocateElement(id element, SEL selector)
{
    if (borrowsTableId(element)) {
        // Qt will still release its children and run the superclass destructor,
        // but must not remove the table interface represented by this borrowed ID.
        const unsigned int noId = 0;
        std::memcpy(reinterpret_cast<char*>(element) + accessibleIdOffset,
                    &noId, sizeof(noId));
    }
    originalDealloc(element, selector);
}

void removeOwnedElements(id cls, SEL selector, NSArray* elements)
{
    NSMutableArray* owned = [[NSMutableArray alloc] init];
    for (id element in elements)
        if (!borrowsTableId(element)) [owned addObject:element];
    originalRemoveElements(cls, selector, owned);
    [owned release];
}
} // namespace

namespace gvt {
bool installQtAccessibilityWorkaround()
{
    static bool installed = false;
    if (installed) return true;
    const auto version = QVersionNumber::fromString(QString::fromLatin1(qVersion()));
    if (QGuiApplication::platformName() != QStringLiteral("cocoa") ||
        (version != QVersionNumber(6, 11, 0) &&
         version != QVersionNumber(6, 11, 1)))
        return false;

    Class cls = objc_lookUpClass("QMacAccessibilityElement");
    Ivar idIvar = cls ? class_getInstanceVariable(cls, "axid") : nullptr;
    Ivar roleIvar = cls ? class_getInstanceVariable(cls, "synthesizedRole") : nullptr;
    Method dealloc = cls ? class_getInstanceMethod(cls, sel_registerName("dealloc")) : nullptr;
    Method remove = cls ? class_getClassMethod(cls, sel_registerName("removeElementsFromCache:")) : nullptr;
    // Never guess a private layout or apply this to an unreviewed Qt release.
    if (!idIvar || !roleIvar || !dealloc || !remove ||
        std::strcmp(ivar_getTypeEncoding(idIvar), @encode(unsigned int)) != 0 ||
        ivar_getTypeEncoding(roleIvar)[0] != '@') {
        qWarning("Qt Cocoa accessibility workaround unavailable: unexpected runtime layout");
        return false;
    }
    synthesizedRoleIvar = roleIvar;
    accessibleIdOffset = ivar_getOffset(idIvar);
    originalDealloc = reinterpret_cast<Dealloc>(
        method_setImplementation(dealloc, reinterpret_cast<IMP>(deallocateElement)));
    originalRemoveElements = reinterpret_cast<RemoveElements>(
        method_setImplementation(remove, reinterpret_cast<IMP>(removeOwnedElements)));
    installed = true;
    return true;
}
} // namespace gvt
