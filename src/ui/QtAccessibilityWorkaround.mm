// Keep Cocoa table refreshes from deleting Qt-owned accessibility interfaces.
// Qt 6.11.0/1 both confuses synthesized rows with the table they borrow an ID
// from and deletes real cells still referenced by QAccessibleTable::childToId.
// Retire native representations only; Qt remains responsible for cell lifetime.
#include "QtAccessibilityWorkaround.h"

#include <QGuiApplication>
#include <QVersionNumber>
#include <QDebug>
#include <QtGui/private/qaccessiblecache_p.h>

#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include <cstring>
#include <dlfcn.h>

namespace {
Ivar synthesizedRoleIvar = nullptr;
ptrdiff_t accessibleIdOffset = 0;
using Dealloc = void (*)(id, SEL);
Dealloc originalDealloc = nullptr;
// Qt exports this native-only eviction method, but declares it private. Resolve
// it only for the reviewed runtime below rather than weakening C++ access checks
// or inserting null cache entries that accumulate after cells are deleted.
using RemoveNativeElement = void (*)(QAccessibleCache*, QAccessible::Id);
RemoveNativeElement removeNativeElement = nullptr;

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

void retireNativeElements(id, SEL, NSArray* elements)
{
    auto* cache = QAccessibleCache::instance();
    for (id element in elements) {
        if (borrowsTableId(element)) continue;
        QAccessible::Id identifier = 0;
        std::memcpy(&identifier,
                    reinterpret_cast<const char*>(element) + accessibleIdOffset,
                    sizeof(identifier));
        if (!identifier || cache->elementForId(identifier) != element) continue;
        // QAccessibleTable owns this cell interface and retains its ID across
        // filters/inserts/removes. Deleting it here leaves a dangling child ID
        // and the next modelChange dereferences null. Replace just the native
        // cache entry: removal invalidates/releases the old Cocoa object
        // (zeroing its ID before dealloc) without deleting the Qt interface.
        // Identity checking also prevents a retired row from evicting a newer
        // native representation already registered for the same surviving cell.
        removeNativeElement(cache, identifier);
    }
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
    auto removeNative = reinterpret_cast<RemoveNativeElement>(dlsym(
        RTLD_DEFAULT, "_ZN16QAccessibleCache23removeAccessibleElementEj"));
    // Never guess a private layout or apply this to an unreviewed Qt release.
    if (!idIvar || !roleIvar || !dealloc || !remove || !removeNative ||
        std::strcmp(ivar_getTypeEncoding(idIvar), @encode(unsigned int)) != 0 ||
        ivar_getTypeEncoding(roleIvar)[0] != '@') {
        qWarning("Qt Cocoa accessibility workaround unavailable: unexpected runtime layout");
        return false;
    }
    synthesizedRoleIvar = roleIvar;
    accessibleIdOffset = ivar_getOffset(idIvar);
    removeNativeElement = removeNative;
    originalDealloc = reinterpret_cast<Dealloc>(
        method_setImplementation(dealloc, reinterpret_cast<IMP>(deallocateElement)));
    method_setImplementation(remove, reinterpret_cast<IMP>(retireNativeElements));
    installed = true;
    return true;
}
} // namespace gvt
