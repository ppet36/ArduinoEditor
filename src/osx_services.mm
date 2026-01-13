#ifdef __APPLE__
#import <AppKit/AppKit.h>
#import <dispatch/dispatch.h>

static void DisableServicesMenuImpl(void) {
    // 1) nastav prázdné services menu (ne nil)
    NSMenu* empty = [[NSMenu alloc] initWithTitle:@""];
    [NSApp setServicesMenu:empty];

    // 2) najdi a schovej menu item, který ukazuje na services submenu
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
                [it setHidden:YES];   // zmizí "Služby/Services" položka
                [it setSubmenu:[[NSMenu alloc] initWithTitle:@""]];
                return;
            }
        }
    }
}

extern "C" void OSXDisableServicesMenu(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        DisableServicesMenuImpl();
        // pro jistotu ještě jednou o tick později (wx/AppKit někdy přepíše menu po startu)
        dispatch_async(dispatch_get_main_queue(), ^{
            DisableServicesMenuImpl();
        });
    });
}
#endif

