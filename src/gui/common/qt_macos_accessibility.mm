#include "qt_macos_accessibility.h"

#include <QGuiApplication>
#include <QDebug>

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <cstring>

namespace {
Ivar idField;
Ivar rowsField;
Ivar columnsField;
Ivar roleField;
IMP superDealloc;

unsigned int accessibleId(id element)
{
    unsigned int value;
    std::memcpy(&value, reinterpret_cast<const char *>(element) +
                       ivar_getOffset(idField), sizeof(value));
    return value;
}

void invalidateElement(id element, SEL)
{
    // Interfaces belong to Qt's cache and item views, not to native wrappers.
    // In particular, synthetic rows/columns borrow their parent table's ID.
    const unsigned int invalidId = 0;
    std::memcpy(reinterpret_cast<char *>(element) + ivar_getOffset(idField),
                &invalidId, sizeof(invalidId));
    for (Ivar field : {rowsField, columnsField}) {
        id children = object_getIvar(element, field);
        object_setIvar(element, field, nil);
        [children autorelease];
    }
    object_setIvar(element, roleField, nil);
    NSAccessibilityPostNotification(element, NSAccessibilityUIElementDestroyedNotification);
}

void destroyElement(id element, SEL selector)
{
    if (accessibleId(element))
        NSAccessibilityPostNotification(element, NSAccessibilityUIElementDestroyedNotification);
    for (Ivar field : {rowsField, columnsField}) {
        id children = object_getIvar(element, field);
        object_setIvar(element, field, nil);
        [children release];
    }
    reinterpret_cast<void (*)(id, SEL)>(superDealloc)(element, selector);
}

bool hasType(Ivar field, const char *type)
{
    return field && std::strcmp(ivar_getTypeEncoding(field), type) == 0;
}
} // namespace

void installMacAccessibilityWorkaround()
{
    // QTBUG-149612, upstream 8148c48deb8e4345bacb797cb75c011df6f797fa.
    // Qt 6.12.0 deletes live table/cell interfaces from invalidate/dealloc.
    // Apply the ownership correction locally without changing Homebrew Qt or
    // disabling accessibility. Do not touch other Qt versions or fixed builds
    // (the upstream fix removes removeElementsFromCache:).
    static bool installed = false;
    if (installed || std::strcmp(qVersion(), "6.12.0") != 0 ||
        QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return;

    Class cls = NSClassFromString(@"QMacAccessibilityElement");
    if (!cls || !class_getClassMethod(cls, sel_registerName("removeElementsFromCache:")))
        return;

    idField = class_getInstanceVariable(cls, "axid");
    rowsField = class_getInstanceVariable(cls, "rows");
    columnsField = class_getInstanceVariable(cls, "columns");
    roleField = class_getInstanceVariable(cls, "synthesizedRole");
    Method invalidate = class_getInstanceMethod(cls, sel_registerName("invalidate"));
    Method dealloc = class_getInstanceMethod(cls, sel_registerName("dealloc"));
    Method parentDealloc = class_getInstanceMethod(class_getSuperclass(cls),
                                                   sel_registerName("dealloc"));
    // Resolve offsets at runtime and refuse an unexpected private class layout.
    if (!hasType(idField, "I") || !rowsField || !columnsField || !roleField ||
        ivar_getTypeEncoding(rowsField)[0] != '@' ||
        ivar_getTypeEncoding(columnsField)[0] != '@' ||
        ivar_getTypeEncoding(roleField)[0] != '@' ||
        !invalidate || !dealloc || !parentDealloc) {
        qWarning("Qt macOS accessibility workaround: unsupported runtime layout");
        return;
    }
    superDealloc = method_getImplementation(parentDealloc);
    method_setImplementation(invalidate, reinterpret_cast<IMP>(invalidateElement));
    method_setImplementation(dealloc, reinterpret_cast<IMP>(destroyElement));
    installed = true;
}
