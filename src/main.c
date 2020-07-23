#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <string.h>
#include <signal.h>

#include <IOKitLib.h>
#include <usb/IOUSBLib.h>
#include <serial/IOSerialKeys.h>
#include <CoreFoundation.h>
#include <IOBSD.h>

#define USB_INTERFACE_COUNT     2
#define USB_INTERFACE_CTRL      "/tmp/cu.cat_ctrl"
#define USB_INTERFACE_PTT       "/tmp/cu.cat__ptt"
#define ARRAY_SZ(x)             (size_t)(sizeof(x) / sizeof(x[0]))

static CFRunLoopRef runLoopCurrent = NULL;
static const unsigned int usbVendorID = 0x10C4;
static const unsigned int usbProductID = 0xEA70;

typedef enum {
    IFACE_CTRL = 0,
    IFACE_PTT,
    /* Sentinel */
    IFACE_MAX
} usb_interface_t;

#pragma pack(push, 1)
typedef struct {
    usb_interface_t type;
    const char *path;
} UsbInterfaces;
#pragma pack(pop)

static UsbInterfaces _usb_interfaces[USB_INTERFACE_COUNT] = {
    [IFACE_CTRL] = {
        .type = IFACE_CTRL,
        .path = USB_INTERFACE_CTRL,
    },
    [IFACE_PTT] = {
        .type = IFACE_PTT,
        .path = USB_INTERFACE_PTT,
    },
};

static void pdebug(const char __attribute__ ((unused)) *fmt, ...)
{
#ifdef DEBUG
    va_list args;

    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
#endif /* DEBUG */
}

static void pout(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
}

static void perr(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

static bool deleteSymLinks(void)
{
    int ret = 0;
    int i = 0;

    for (i = 0; i < USB_INTERFACE_COUNT; i++) {
        if (!access(_usb_interfaces[i].path, F_OK)) {
            ret = unlink(_usb_interfaces[i].path);
            if (ret < 0) {
                perr("Unable to delete %s\n", _usb_interfaces[i].path);
                return false;
            }
        }
    }

    return true;
}

static bool createSymlink(const char *path, UsbInterfaces *iface)
{
    int ret = 0;

    if ((path == NULL) && (iface == NULL)) {
        return false;
    }

    ret = symlink(path, iface->path);
    if (ret < 0) {
        perr("Unable to create a symlink: %s -> %s\n",
             path, iface->path);
        return false;
    }

    pout("Symlink created: %s -> %s\n", path, iface->path);
    return true;
}

static void usbDeviceAdded(void __attribute__ ((unused)) *ref,
                           io_iterator_t iterator)
{
    io_service_t dev;
    char path[MAXPATHLEN];
    CFIndex len = (CFIndex)ARRAY_SZ(path);
    char items[USB_INTERFACE_COUNT][MAXPATHLEN];
    char *ptr;
    int i = 0;
    bool found = false;

    while ((dev = IOIteratorNext(iterator))) {
        io_service_t childIterator;
        kern_return_t ret = KERN_FAILURE;

        ret = IORegistryEntryGetChildIterator(dev, kIOServicePlane, &childIterator);
        if (ret != kIOReturnSuccess) {
            continue;
        }

        /*
         * We've got parent USB node, search for children.
         */
        io_registry_entry_t child;
        unsigned char numEntries = 0;

        while ((child = IOIteratorNext(childIterator))) {
            ptr = &items[numEntries][0];

            CFTypeRef bsdPathAsCFString =
                    IORegistryEntrySearchCFProperty(child, kIOServicePlane,
                                                    CFSTR(kIOCalloutDeviceKey),
                                                    kCFAllocatorDefault,
                                                    kIORegistryIterateRecursively);
            if (bsdPathAsCFString) {
                bool result;

                /*
                 * Convert the path from a CFString to a C (NUL-terminated) string for use
                 * with the POSIX open() call.
                 */
                result = CFStringGetCString(bsdPathAsCFString, path, len,
                                            kCFStringEncodingUTF8);
                CFRelease(bsdPathAsCFString);

                if (result) {
                    /*
                     * We've got a path, should verify it and create symbolic
                     * links for it.
                     */

                    // Check path/file exists
                    if (access(path, F_OK) < 0) {
                        perr("%s doesn't exist\n", path);
                        return;
                    }

                    // Check read/write permissions
                    if (access(path, R_OK | W_OK) < 0) {
                        perr("%s is not accessible\n", path);
                        return;
                    }

                    memset(ptr, 0, MAXPATHLEN);
                    memcpy(ptr, path, strlen(path));
                    numEntries++;
                    pdebug("%s\n", path);
                }
            }
        }

        if (numEntries == USB_INTERFACE_COUNT) {
            found = true;
            break;
        }
    }

    if (found) {
        bool status = false;

        // Delete previously created symlinks (if any)
        if (!deleteSymLinks()) {
            perr("Critical error!\n");
            kill(getpid(), SIGTERM);
        }

        // Walk through the list and create symlinks
        for (i = 0; i < USB_INTERFACE_COUNT; i++) {
            status = createSymlink(items[i], &_usb_interfaces[i]);
            if (!status) {
                perr("Critical error!\n");
                kill(getpid(), SIGTERM);
            }
        }

        if (status) {
            pout("SCU-17 interface is ready\n");
            return;
        }
    }
}

static void usbDeviceRemoved(void __attribute__ ((unused)) *ref,
                             io_iterator_t iterator)
{
    io_service_t dev;
    kern_return_t ret = KERN_FAILURE;

    while ((dev = IOIteratorNext(iterator))) {
        ret = IOObjectRelease(dev);
        if (ret != kIOReturnSuccess) {
            perr("[%08x] Unable to release device\n", ret);
        } else {
            if (!deleteSymLinks()) {
                perr("Critical error!\n");
                kill(getpid(), SIGTERM);
            }
            pout("SCU-17 interface released\n", ret);
        }
    }
}

static void sig_handler(int signal)
{
    switch (signal) {
    case SIGTERM:
    case SIGQUIT:
    case SIGABRT:
    case SIGINT:
    default:
        CFRunLoopStop(runLoopCurrent);
        // Clean previously created symlinks
        deleteSymLinks();

        pout("Listener is destroyed\n");
        exit(EXIT_SUCCESS);

        break;
    }
}

int main(void)
{
    CFMutableDictionaryRef mDict;
    IONotificationPortRef notifyPort;
    CFRunLoopSourceRef runLoop;
    mach_port_t mPort;
    io_iterator_t iUsbAdded;
    io_iterator_t iUsbRemoved;
    kern_return_t ret = KERN_FAILURE;

    // Setup signal handler
    signal(SIGTERM, sig_handler);
    signal(SIGQUIT, sig_handler);
    signal(SIGABRT, sig_handler);
    signal(SIGINT, sig_handler);

    // Clean previously created symlinks (if any)
    deleteSymLinks();

    // Create a master port which later will be used for notification purposes
    ret = IOMasterPort(MACH_PORT_NULL, &mPort);
    if (ret || !mPort) {
        perr("[%08x] Can't create a master IOKit port\n", ret);
        return ret;
    }

    // Create matching service
    mDict = IOServiceMatching(kIOUSBDeviceClassName);
    if (mDict == NULL) {
        return KERN_FAILURE;
    }

    // Create matching pattern: VendorID
    CFDictionarySetValue(mDict, CFSTR(kUSBVendorID),
                         CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type,
                                        &usbVendorID));
    // Create matching pattern: ProductID
    CFDictionarySetValue(mDict, CFSTR(kUSBProductID),
                         CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type,
                                        &usbProductID));

    // Create notification port
    notifyPort = IONotificationPortCreate(mPort);
    runLoop = IONotificationPortGetRunLoopSource(notifyPort);
    runLoopCurrent = CFRunLoopGetCurrent();
    if (runLoopCurrent == NULL) {
        perr("Unable to start monitoring loop\n");
        kill(getpid(), SIGTERM);
    }
    CFRunLoopAddSource(runLoopCurrent, runLoop, kCFRunLoopDefaultMode);

    /*
     * Retain additional dictionary references because each call to
     * IOServiceAddMatchingNotification consumes one reference
     */
    mDict = (CFMutableDictionaryRef)CFRetain(mDict);
    mDict = (CFMutableDictionaryRef)CFRetain(mDict);
    mDict = (CFMutableDictionaryRef)CFRetain(mDict);

    // Device inserted handler
    ret = IOServiceAddMatchingNotification(notifyPort, kIOMatchedNotification,
                                           mDict, usbDeviceAdded, NULL,
                                           &iUsbAdded);
    /*
     * In case the device is already in the system, we need to
     * perform discovery and symlink tasks before the run loop
     * is started, so just call the callback for the first time.
     * Later it will be called automatically. The same action is
     * performed for device removal callback.
     */
    usbDeviceAdded(NULL, iUsbAdded);

    // Device removed handler
    ret = IOServiceAddMatchingNotification(notifyPort, kIOTerminatedNotification,
                                           mDict, usbDeviceRemoved, NULL,
                                           &iUsbRemoved);
    usbDeviceRemoved(NULL, iUsbRemoved);

    // Notify the world we're on-line
    if (!ret) {
        pout("Listener is active...\n");
    } else {
        perr("Unable to set listener hooks\n");
        kill(getpid(), SIGTERM);
    }

    // This will run forewer until we break the loop by some interrupt
    CFRunLoopRun();
    return EXIT_SUCCESS;
}
