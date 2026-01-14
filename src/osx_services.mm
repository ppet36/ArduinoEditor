#ifdef __APPLE__
#import <AppKit/AppKit.h>
#import <dispatch/dispatch.h>

static void DisableServicesMenuImpl(void) {
    // 1) setting empty services menu (not nil)
    NSMenu* empty = [[NSMenu alloc] initWithTitle:@""];
    [NSApp setServicesMenu:empty];

    // 2) find and hide the menu item that points to the services submenu
    NSMenu* main = [NSApp mainMenu];
    if (!main) return;

    NSMenu* svc = [NSApp servicesMenu];
    if (!svc) return;

    for (NSMenuItem* top in [main itemArray]) {
        NSMenu* sub = [top submenu];
        if (!sub) continue;

        for (NSMenuItem* it in [sub itemArray]) {
            if ([it submenu] == svc) {
                [it setEnabled:NO];
                [it setHidden:YES];   // "Services" item disappears
                [it setSubmenu:[[NSMenu alloc] initWithTitle:@""]];
                return;
            }
        }
    }
}

extern "C" void OSXDisableServicesMenu(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        DisableServicesMenuImpl();
        // to be sure, one more tick later (wx/AppKit sometimes overwrites the menu after startup)
        dispatch_async(dispatch_get_main_queue(), ^{
            DisableServicesMenuImpl();
        });
    });
}
#endif

