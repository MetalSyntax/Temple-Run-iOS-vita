// UIKit + QuartzCore + OpenGLES (EAGL) + CoreGraphics + MediaPlayer mínimos sobre objc.c.
//
// Ciclo de vida como en iPhone OS 2/3:
//   main() del juego -> UIApplicationMain -> nib (si hay, precompilado en nib.txt por
//   tools/nib_prep.py) -> applicationDidFinishLaunching: -> layoutSubviews de las vistas ->
//   run loop (ui_runloop_tick: input, acelerómetro, timers, notificaciones diferidas, IME).
// El juego dibuja desde un NSTimer/CADisplayLink/hilo propio y presenta con
// -[EAGLContext presentRenderbuffer:] -> gles_present().
#include <math.h>
#include <png.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/motion.h>
#include <psp2/touch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <vitaGL.h>

#include "cgbi.h"
#include "foundation.h"
#include "gles.h"
#include "hle.h"
#include "imports.h"
#include "input.h"
#include "log.h"
#include "macho.h"
#include "objc.h"
#include "options.h"

typedef struct {
    float x, y;
} CGPoint;
typedef struct {
    float w, h;
} CGSize;
typedef struct {
    CGPoint o;
    CGSize s;
} CGRect;
typedef struct {
    float a, b, c, d, tx, ty;
} CGAffineTransform;

#define AR(x) objc_autorelease((id)(x))
static const CGAffineTransform IDENTITY = {1, 0, 0, 1, 0, 0};

static objc_class *c_UIApplication, *c_UIView, *c_UIWindow, *c_CALayer, *c_CAEAGLLayer, *c_EAGLContext, *c_UIScreen,
    *c_UIDevice, *c_UIAccelerometer, *c_UIAcceleration, *c_UITouch, *c_UIEvent, *c_UIImage, *c_UIColor, *c_UIFont,
    *c_UIAlertView, *c_UITextField, *c_UIViewController, *c_MPMoviePlayerController, *c_MPMusicPlayerController,
    *c_UINibDecoder;
static id the_app, app_delegate, main_screen, cur_device, accel_obj;

// ================================================================ UIView / CALayer

typedef struct {
    CGRect frame;
    CGAffineTransform transform;
    id layer, superview, subviews, delegate;
    int hidden, tag, user_interaction, multitouch, needs_layout;
    float alpha;
} view_ext;
// + 64 bytes para estado de subclases nativas (GLKView, GLKViewController)
static view_ext *VX(id v) { return objc_ext(v, sizeof(view_ext) + 64); }

#define MAX_LAYOUT 64
static id layout_queue[MAX_LAYOUT];
static int nlayout;
static void need_layout(id v) {
    for (int i = 0; i < nlayout; i++)
        if (layout_queue[i] == v)
            return;
    if (nlayout < MAX_LAYOUT)
        layout_queue[nlayout++] = v;
}

static id v_make_layer(id self) {
    objc_class *lc = (objc_class *)MSG0(object_getClass(self), "layerClass");
    id layer = MSG0(objc_alloc(lc ? lc : c_CALayer), "init");
    view_ext *lx = VX(layer);
    lx->delegate = self; // la vista dueña (bounds del drawable)
    return layer;
}
static id v_initWithFrame(id self, SEL _c, CGRect f) {
    view_ext *x = VX(self);
    x->frame = f;
    x->transform = IDENTITY;
    x->alpha = 1;
    x->user_interaction = 1;
    x->subviews = ns_array_new();
    x->layer = v_make_layer(self);
    VX(x->layer)->frame = (CGRect){{0, 0}, f.s};
    l_debug("UIView initWithFrame %s %.0fx%.0f", class_getName(object_getClass(self)), f.s.w, f.s.h);
    return self;
}
static id v_init(id self, SEL _c) {
    CGRect r = {{0, 0}, {0, 0}};
    return v_initWithFrame(self, _c, r);
}
// Nib: la vista se crea con initWithCoder:; nuestro "coder" trae el frame del nib.
typedef struct {
    objc_class *isa;
    CGRect frame;
} nib_coder;
static id v_initWithCoder(id self, SEL _c, nib_coder *coder) { return v_initWithFrame(self, _c, coder->frame); }
static id v_layerClass(id cls, SEL _c) { return c_CALayer; }
static void v_frame(CGRect *ret, id self, SEL _c) { *ret = VX(self)->frame; }
static void v_setFrame(id self, SEL _c, CGRect f) {
    view_ext *x = VX(self);
    int changed = f.s.w != x->frame.s.w || f.s.h != x->frame.s.h;
    x->frame = f;
    if (x->layer)
        VX(x->layer)->frame = (CGRect){{0, 0}, f.s};
    if (changed)
        need_layout(self);
}
static void v_bounds(CGRect *ret, id self, SEL _c) { *ret = (CGRect){{0, 0}, VX(self)->frame.s}; }
static void v_setBounds(id self, SEL _c, CGRect b) {
    view_ext *x = VX(self);
    CGRect f = x->frame;
    f.o.x += (f.s.w - b.s.w) / 2; // centrado como UIKit
    f.o.y += (f.s.h - b.s.h) / 2;
    f.s = b.s;
    v_setFrame(self, _c, f);
}
static void v_center(CGPoint *ret, id self, SEL _c) {
    CGRect f = VX(self)->frame;
    *ret = (CGPoint){f.o.x + f.s.w / 2, f.o.y + f.s.h / 2};
}
static void v_setCenter(id self, SEL _c, CGPoint c) {
    view_ext *x = VX(self);
    x->frame.o = (CGPoint){c.x - x->frame.s.w / 2, c.y - x->frame.s.h / 2};
}
static void v_transform(CGAffineTransform *ret, id self, SEL _c) { *ret = VX(self)->transform; }
static void v_setTransform(id self, SEL _c, CGAffineTransform t) { VX(self)->transform = t; }
static id v_layer(id self, SEL _c) { return VX(self)->layer; }
static id v_window(id self, SEL _c) {
    id v = self;
    while (v && !objc_is_kind(v, c_UIWindow))
        v = VX(v)->superview;
    return v;
}
static void v_addSubview(id self, SEL _c, id sub) {
    if (!sub || sub == self)
        return;
    view_ext *s = VX(sub);
    if (s->superview == self)
        return;
    s->superview = self;
    ns_array_add(VX(self)->subviews, sub);
    need_layout(sub);
    if (objc_responds(sub, sel_registerName("didMoveToWindow")) && v_window(self, NULL))
        MSG0(sub, "didMoveToWindow");
    l_debug("addSubview %s -> %s", class_getName(object_getClass(sub)), class_getName(object_getClass(self)));
}
static void v_removeFromSuperview(id self, SEL _c) {
    view_ext *x = VX(self);
    if (!x->superview)
        return;
    id sup = x->superview;
    x->superview = NULL;
    MSG1(VX(sup)->subviews, "removeObject:", self);
}
static id v_superview(id self, SEL _c) { return VX(self)->superview; }
static id v_subviews(id self, SEL _c) { return VX(self)->subviews; }
static void v_setHidden(id self, SEL _c, int h) { VX(self)->hidden = h & 0xFF; }
static int v_isHidden(id self, SEL _c) { return VX(self)->hidden; }
static void v_setAlpha(id self, SEL _c, float a) { VX(self)->alpha = a; }
static float v_alpha(id self, SEL _c) { return VX(self)->alpha; }
static void v_setTag(id self, SEL _c, int t) { VX(self)->tag = t; }
static int v_tag(id self, SEL _c) { return VX(self)->tag; }
static id v_viewWithTag(id self, SEL _c, int t) {
    if (VX(self)->tag == t)
        return self;
    id subs = VX(self)->subviews;
    for (int i = 0; i < ns_array_count(subs); i++) {
        id r = v_viewWithTag(ns_array_get(subs, i), _c, t);
        if (r)
            return r;
    }
    return NULL;
}
static void v_setUserInteraction(id self, SEL _c, int on) { VX(self)->user_interaction = on & 0xFF; }
static int v_userInteraction(id self, SEL _c) { return VX(self)->user_interaction; }
static void v_setMultipleTouch(id self, SEL _c, int on) { VX(self)->multitouch = on & 0xFF; }
static void v_setNeedsLayout(id self, SEL _c) { need_layout(self); }
static void v_noop(id self, SEL _c) {}
static int v_yes(id self, SEL _c) { return 1; }
static int v_no(id self, SEL _c) { return 0; }
static void v_bringToFront(id self, SEL _c, id sub) {
    id subs = VX(self)->subviews;
    objc_retain(sub);
    MSG1(subs, "removeObject:", sub);
    ns_array_add(subs, sub);
    objc_release(sub);
}
static void v_convertPoint(CGPoint *ret, id self, SEL _c, CGPoint p, id view) { *ret = p; }
static void v_convertRect(CGRect *ret, id self, SEL _c, CGRect r, id view) { *ret = r; }
static float v_scale(id self, SEL _c) { return 1.0f; }
static void v_sizeThatFits(CGSize *ret, id self, SEL _c, CGSize s) { *ret = VX(self)->frame.s; }

static double now_s(void) { return sceKernelGetProcessTimeWide() / 1e6; }
// +[UIView beginAnimations:...commitAnimations]: los cambios ya se aplicaron al instante; el delegado recibe
// animationDidStop:finished:context: cuando habría terminado la animación (el juego espera ese aviso).
#define MAX_ANIM 16
static struct {
    id delegate, anim_id;
    SEL sel;
    void *ctx;
    double at;
} anims[MAX_ANIM], cur_anim;
static int nanims;
static double cur_dur = 0.2, cur_delay;
static void va_begin(id cls, SEL _c, id anim_id, void *ctx) {
    memset(&cur_anim, 0, sizeof(cur_anim));
    cur_anim.anim_id = anim_id, cur_anim.ctx = ctx, cur_dur = 0.2, cur_delay = 0;
}
static void va_duration(id cls, SEL _c, double d) { cur_dur = d; }
static void va_delay(id cls, SEL _c, double d) { cur_delay = d; }
static void va_delegate(id cls, SEL _c, id d) { cur_anim.delegate = d; }
static void va_didStop(id cls, SEL _c, SEL s) { cur_anim.sel = s; }
static void va_commit(id cls, SEL _c) {
    if (!cur_anim.delegate || !cur_anim.sel || nanims >= MAX_ANIM)
        return;
    cur_anim.at = now_s() + cur_delay + cur_dur;
    objc_retain(cur_anim.delegate), objc_retain(cur_anim.anim_id);
    anims[nanims++] = cur_anim;
}
static void anims_tick(void) {
    double t = now_s();
    for (int i = 0; i < nanims;)
        if (t >= anims[i].at) {
            typeof(anims[0]) a = anims[i];
            anims[i] = anims[--nanims];
            objc_msg(a.delegate, a.sel, (uint32_t)a.anim_id, (uint32_t)ns_num_int(1), (uint32_t)a.ctx, 0);
            objc_release(a.delegate), objc_release(a.anim_id);
        } else {
            i++;
        }
}
static const nmethod uiview_c[] = {
    NM("layerClass", v_layerClass),
    NM("beginAnimations:context:", va_begin), NM("setAnimationDuration:", va_duration),
    NM("setAnimationDelay:", va_delay), NM("setAnimationDelegate:", va_delegate),
    NM("setAnimationDidStopSelector:", va_didStop), NM("setAnimationWillStartSelector:", v_noop),
    NM("setAnimationCurve:", v_noop), NM("setAnimationBeginsFromCurrentState:", v_noop),
    NM("setAnimationRepeatCount:", v_noop), NM("setAnimationsEnabled:", v_noop), NM("commitAnimations", va_commit), {0}};
static const nmethod uiview_i[] = {
    NM("init", v_init), NM("initWithFrame:", v_initWithFrame), NM("initWithCoder:", v_initWithCoder),
    NM("frame", v_frame), NM("setFrame:", v_setFrame), NM("bounds", v_bounds), NM("setBounds:", v_setBounds),
    NM("center", v_center), NM("setCenter:", v_setCenter), NM("transform", v_transform),
    NM("setTransform:", v_setTransform), NM("layer", v_layer), NM("window", v_window), NM("addSubview:", v_addSubview),
    NM("insertSubview:atIndex:", v_addSubview), NM("insertSubview:aboveSubview:", v_addSubview),
    NM("insertSubview:belowSubview:", v_addSubview), NM("removeFromSuperview", v_removeFromSuperview),
    NM("superview", v_superview), NM("subviews", v_subviews), NM("setHidden:", v_setHidden), NM("isHidden", v_isHidden),
    NM("hidden", v_isHidden), NM("setAlpha:", v_setAlpha), NM("alpha", v_alpha), NM("setTag:", v_setTag),
    NM("tag", v_tag), NM("viewWithTag:", v_viewWithTag), NM("setUserInteractionEnabled:", v_setUserInteraction),
    NM("isUserInteractionEnabled", v_userInteraction), NM("setMultipleTouchEnabled:", v_setMultipleTouch),
    NM("setExclusiveTouch:", v_noop), NM("setNeedsLayout", v_setNeedsLayout), NM("layoutIfNeeded", v_noop),
    NM("layoutSubviews", v_noop), NM("setNeedsDisplay", v_noop), NM("setNeedsDisplayInRect:", v_noop),
    NM("setOpaque:", v_noop), NM("setBackgroundColor:", v_noop), NM("setClearsContextBeforeDrawing:", v_noop),
    NM("setAutoresizingMask:", v_noop), NM("setAutoresizesSubviews:", v_noop), NM("setClipsToBounds:", v_noop),
    NM("setContentMode:", v_noop), NM("setContentScaleFactor:", v_noop), NM("contentScaleFactor", v_scale),
    NM("bringSubviewToFront:", v_bringToFront), NM("sendSubviewToBack:", v_noop), NM("convertPoint:toView:", v_convertPoint),
    NM("convertPoint:fromView:", v_convertPoint), NM("convertRect:toView:", v_convertRect),
    NM("convertRect:fromView:", v_convertRect), NM("becomeFirstResponder", v_yes), NM("resignFirstResponder", v_yes),
    NM("canBecomeFirstResponder", v_yes), NM("isFirstResponder", v_no), NM("sizeToFit", v_noop),
    NM("sizeThatFits:", v_sizeThatFits), NM("didMoveToWindow", v_noop), NM("setAccessibilityLabel:", v_noop), {0}};

// CALayer: frame propio; CAEAGLLayer guarda drawableProperties.
static id l_init(id self, SEL _c) {
    VX(self)->transform = IDENTITY;
    return self;
}
static void l_bounds(CGRect *ret, id self, SEL _c) { *ret = (CGRect){{0, 0}, VX(self)->frame.s}; }
static void l_setFrame(id self, SEL _c, CGRect f) { VX(self)->frame = f; }
static void l_setDrawableProperties(id self, SEL _c, id d) { VX(self)->subviews = objc_retain(d); }
static id l_drawableProperties(id self, SEL _c) { return VX(self)->subviews; }
static float l_one(id self, SEL _c) { return 1.0f; }
static const nmethod calayer_i[] = {
    NM("init", l_init), NM("bounds", l_bounds), NM("frame", l_bounds), NM("setFrame:", l_setFrame),
    NM("setBounds:", l_setFrame), NM("setOpaque:", v_noop), NM("setDrawableProperties:", l_setDrawableProperties),
    NM("drawableProperties", l_drawableProperties), NM("setContentsScale:", v_noop), NM("contentsScale", l_one),
    NM("setTransform:", v_noop), NM("setAffineTransform:", v_noop), NM("setNeedsDisplay", v_noop),
    NM("setMasksToBounds:", v_noop), NM("setBackgroundColor:", v_noop), NM("addSublayer:", v_noop),
    NM("setPosition:", v_noop), NM("setHidden:", v_noop), NM("setPresentsWithTransaction:", v_noop), {0}};

// ================================================================ UIWindow / UIScreen / UIApplication

static id key_window, nib_window;
static void w_makeKeyAndVisible(id self, SEL _c) {
    key_window = self;
    id subs = VX(self)->subviews;
    for (int i = 0; i < ns_array_count(subs); i++)
        need_layout(ns_array_get(subs, i));
    l_info("UIWindow makeKeyAndVisible (%d subvistas)", ns_array_count(subs));
}
// viewWillAppear:/viewDidAppear: como UIKit al mostrar un controller (varios motores arrancan ahí)
static void vc_appear(id vc) {
    if (objc_responds(vc, sel_registerName("viewWillAppear:")))
        MSG1(vc, "viewWillAppear:", 0);
    if (objc_responds(vc, sel_registerName("viewDidAppear:")))
        MSG1(vc, "viewDidAppear:", 0);
}
static void w_setRootViewController(id self, SEL _c, id vc) {
    VX(self)->delegate = objc_retain(vc);
    id v = (id)MSG0(vc, "view");
    if (v)
        v_addSubview(self, _c, v);
    vc_appear(vc);
}
static id w_rootViewController(id self, SEL _c) { return VX(self)->delegate; }
static id w_screen(id self, SEL _c) { return main_screen; }
static const nmethod uiwindow_i[] = {
    NM("makeKeyAndVisible", w_makeKeyAndVisible), NM("makeKeyWindow", w_makeKeyAndVisible),
    NM("setRootViewController:", w_setRootViewController), NM("rootViewController", w_rootViewController),
    NM("screen", w_screen), NM("setScreen:", v_noop), NM("setWindowLevel:", v_noop), {0}};

static id sc_main(id cls, SEL _c) { return main_screen; }
static id sc_screens(id cls, SEL _c) {
    id a = ns_array_new();
    ns_array_add(a, main_screen);
    return AR(a);
}
#ifndef PORT_SCREEN_W
#define PORT_SCREEN_W 320
#define PORT_SCREEN_H 480
#define PORT_SCREEN_SCALE 1.0f
#endif
static void sc_bounds(CGRect *ret, id self, SEL _c) { *ret = (CGRect){{0, 0}, {PORT_SCREEN_W, PORT_SCREEN_H}}; }
static void sc_appFrame(CGRect *ret, id self, SEL _c) { *ret = (CGRect){{0, 0}, {PORT_SCREEN_W, PORT_SCREEN_H}}; }
static float sc_scale(id self, SEL _c) { return PORT_SCREEN_SCALE; }
static const nmethod uiscreen_c[] = {NM("mainScreen", sc_main), NM("screens", sc_screens), {0}};
static const nmethod uiscreen_i[] = {
    NM("bounds", sc_bounds), NM("applicationFrame", sc_appFrame), NM("scale", sc_scale),
    NM("nativeBounds", sc_bounds), NM("nativeScale", sc_scale), NM("brightness", l_one), NM("setBrightness:", v_noop), {0}};

static int status_orientation = 3; // UIInterfaceOrientationLandscapeRight
static id ap_shared(id cls, SEL _c) { return the_app; }
static id ap_delegate(id self, SEL _c) { return app_delegate; }
static void ap_setDelegate(id self, SEL _c, id d) { app_delegate = d; }
static int ap_statusOrientation(id self, SEL _c) { return status_orientation; }
// gles.c: con la barra en retrato (1/2) el juego dibuja en retrato (p. ej. "Crear perfil" + teclado) -> no girar.
int ui_status_portrait(void) { return status_orientation == 1 || status_orientation == 2; }
static void ap_setStatusOrientation(id self, SEL _c, int o) {
    status_orientation = o;
    l_info("setStatusBarOrientation:%d", o);
}
static id ap_keyWindow(id self, SEL _c) { return key_window; }
static id ap_windows(id self, SEL _c) {
    id a = ns_array_new();
    if (key_window)
        ns_array_add(a, key_window);
    return AR(a);
}
static int ap_openURL(id self, SEL _c, id url) {
    l_info("openURL: %s (ignorado)", ns_cstr(url));
    return 0;
}
static int ap_zero(id self, SEL _c) { return 0; }
static const nmethod uiapp_c[] = {NM("sharedApplication", ap_shared), {0}};
static const nmethod uiapp_i[] = {
    NM("delegate", ap_delegate), NM("setDelegate:", ap_setDelegate), NM("statusBarOrientation", ap_statusOrientation),
    NM("setStatusBarOrientation:", ap_setStatusOrientation), NM("setStatusBarOrientation:animated:", ap_setStatusOrientation),
    NM("setStatusBarHidden:", v_noop), NM("setStatusBarHidden:animated:", v_noop),
    NM("setStatusBarHidden:withAnimation:", v_noop), NM("setStatusBarStyle:", v_noop),
    NM("setStatusBarStyle:animated:", v_noop), NM("setIdleTimerDisabled:", v_noop), NM("isIdleTimerDisabled", v_yes),
    NM("keyWindow", ap_keyWindow), NM("windows", ap_windows), NM("openURL:", ap_openURL), NM("canOpenURL:", ap_zero),
    NM("applicationState", ap_zero), NM("setNetworkActivityIndicatorVisible:", v_noop),
    NM("registerForRemoteNotificationTypes:", v_noop), NM("setApplicationIconBadgeNumber:", v_noop),
    NM("applicationIconBadgeNumber", ap_zero), NM("cancelAllLocalNotifications", v_noop),
    NM("beginIgnoringInteractionEvents", v_noop), NM("endIgnoringInteractionEvents", v_noop),
    NM("isStatusBarHidden", v_yes), NM("statusBarFrame", sc_appFrame), NM("beginBackgroundTaskWithExpirationHandler:", ap_zero),
    NM("endBackgroundTask:", v_noop), NM("setApplicationSupportsShakeToEdit:", v_noop), {0}};

// ================================================================ EAGLContext

static id cur_context;
static id ea_initWithAPI(id self, SEL _c, int api) {
    VX(self)->tag = api;
    l_info("EAGLContext initWithAPI:%d", api);
    if (api == 2)
        gles_api = 2;
    else if (api != 1)
        return NULL; // GLES3 (iOS 7): el juego cae a GLES2
    return self;
}
static int ea_setCurrent(id cls, SEL _c, id ctx) {
    cur_context = ctx;
    return 1;
}
static id ea_current(id cls, SEL _c) { return cur_context; }
static int ea_renderbufferStorage(id self, SEL _c, int target, id layer) {
    CGRect b = VX(layer)->frame;
    int w = (int)b.s.w, h = (int)b.s.h;
    if (w <= 0 || h <= 0)
        w = 320, h = 480;
    l_info("renderbufferStorage:fromDrawable: %dx%d", w, h);
    gles_drawable_storage(w, h);
    return 1;
}
static uint32_t frames;
static volatile int in_present; // para el log "vivo": distingue cuelgue en present de cuelgue en el juego
static int ea_present(id self, SEL _c, int target) {
    in_present = 1;
    gles_present();
    in_present = 0;
    frames++;
    return 1;
}
static int ea_api(id self, SEL _c) { return VX(self)->tag; }
static id ea_sharegroup(id self, SEL _c) { return self; }
static id ea_initWithAPIShare(id self, SEL _c, int api, id share) { return ea_initWithAPI(self, _c, api); }
static const nmethod eagl_c[] = {NM("setCurrentContext:", ea_setCurrent), NM("currentContext", ea_current), {0}};
static const nmethod eagl_i[] = {
    NM("initWithAPI:", ea_initWithAPI), NM("initWithAPI:sharegroup:", ea_initWithAPIShare),
    NM("renderbufferStorage:fromDrawable:", ea_renderbufferStorage), NM("presentRenderbuffer:", ea_present),
    NM("API", ea_api), NM("sharegroup", ea_sharegroup), {0}};

// ================================================================ UIDevice

static id dv_current(id cls, SEL _c) { return cur_device; }
static id dv_model(id self, SEL _c) { return ns_str("iPhone"); }
static id dv_systemName(id self, SEL _c) { return ns_str("iPhone OS"); }
static id dv_systemVersion(id self, SEL _c) { return ns_str(PORT_SYSTEM_VERSION); }
static id dv_udid(id self, SEL _c) { return ns_str("0123456789abcdef0123456789abcdef01234567"); }
static id dv_name(id self, SEL _c) { return ns_str("PS Vita"); }
// UIDeviceOrientation: 3 = LandscapeLeft (home a la DERECHA = UIInterfaceOrientationLandscapeRight), 4 = home a la
// izquierda. Rotación 1 muestra home a la derecha (igual que el acelerómetro simulado) -> 3.
static int dv_orientation(id self, SEL _c) { return gles_rotation() == 1 ? 3 : gles_rotation() == 2 ? 4 : 1; }
static float dv_battery(id self, SEL _c) { return 1.0f; }
static const nmethod uidevice_c[] = {NM("currentDevice", dv_current), {0}};
static const nmethod uidevice_i[] = {
    NM("model", dv_model), NM("localizedModel", dv_model), NM("systemName", dv_systemName),
    NM("systemVersion", dv_systemVersion), NM("uniqueIdentifier", dv_udid), NM("name", dv_name),
    NM("orientation", dv_orientation), NM("beginGeneratingDeviceOrientationNotifications", v_noop),
    NM("endGeneratingDeviceOrientationNotifications", v_noop), NM("isMultitaskingSupported", v_no),
    NM("userInterfaceIdiom", ap_zero), NM("batteryLevel", dv_battery), NM("batteryState", ap_zero),
    NM("setBatteryMonitoringEnabled:", v_noop), NM("setProximityMonitoringEnabled:", v_noop),
    NM("isGeneratingDeviceOrientationNotifications", v_yes), {0}};

// ================================================================ acelerómetro (stick izquierdo)

typedef struct {
    objc_class *isa;
    double x, y, z, t;
} ui_accel;
static id acc_delegate;
static double acc_interval = 1.0 / 30, acc_next;
static id ac_shared(id cls, SEL _c) { return accel_obj; }
static void ac_setDelegate(id self, SEL _c, id d) {
    acc_delegate = d;
    l_info("UIAccelerometer delegate = %s", d ? class_getName(object_getClass(d)) : "nil");
}
static id ac_delegate(id self, SEL _c) { return acc_delegate; }
static void ac_setInterval(id self, SEL _c, double i) { acc_interval = i > 0.005 ? i : 0.005; }
static double ac_interval(id self, SEL _c) { return acc_interval; }
static double ua_x(ui_accel *self, SEL _c) { return self->x; }
static double ua_y(ui_accel *self, SEL _c) { return self->y; }
static double ua_z(ui_accel *self, SEL _c) { return self->z; }
static double ua_t(ui_accel *self, SEL _c) { return self->t; }
static const nmethod uiaccm_c[] = {NM("sharedAccelerometer", ac_shared), {0}};
static const nmethod uiaccm_i[] = {
    NM("setDelegate:", ac_setDelegate), NM("delegate", ac_delegate), NM("setUpdateInterval:", ac_setInterval),
    NM("updateInterval", ac_interval), {0}};
static const nmethod uiaccel_i[] = {NM("x", ua_x), NM("y", ua_y), NM("z", ua_z), NM("timestamp", ua_t), {0}};

// Gravedad en ejes de la pantalla de la Vita (x derecha, y arriba, z hacia el jugador) a partir del
// stick, y de ahí a los ejes del iPhone según cómo está girado el drawable (ver gles_present).
static void accel_tick(void) {
    if (!acc_delegate)
        return;
    double now = sceKernelGetProcessTimeWide() / 1e6;
    if (now < acc_next)
        return;
    acc_next = now + acc_interval;
    float sx, sy;
    input_stick(&sx, &sy);
    double roll = sx * 0.6, pitch = 0.5 + sy * 0.5; // 0,5 rad = como se sostiene el teléfono
    double gx = sin(roll) * cos(pitch), gy = -cos(roll) * cos(pitch), gz = -sin(pitch);
    // Sensor real (en g, mismos ejes de pantalla); el stick movido manda, y si el sensor falla queda el stick.
    static int motion = -1; // -1 sin iniciar, 0 falló, 1 activo
    if (opt_accel && motion < 0) {
        int r = sceMotionStartSampling();
        motion = r >= 0;
        l_info("sceMotionStartSampling -> 0x%08X (%s)", r, motion ? "sensor activo" : "uso el stick");
    }
    SceMotionSensorState ms;
    if (opt_accel && motion > 0 && sx == 0 && sy == 0 && sceMotionGetSensorState(&ms, 1) >= 0)
        gx = ms.accelerometer.x, gy = ms.accelerometer.y, gz = ms.accelerometer.z;
    ui_accel *a = objc_alloc(c_UIAcceleration);
    switch (gles_rotation()) {
    case 1: // landscape-right: +y del iPhone = izquierda de la pantalla, +x = arriba
        a->x = gy, a->y = -gx;
        break;
    case 2:
        a->x = -gy, a->y = gx;
        break;
    default:
        a->x = gx, a->y = gy;
    }
    a->z = gz;
    a->t = now;
    MSG2(acc_delegate, "accelerometer:didAccelerate:", accel_obj, a);
    objc_release(a);
}

// ================================================================ toques

typedef struct {
    objc_class *isa;
    CGPoint loc, prev;
    int phase, taps;
    double t;
    id view;
} ui_touch;
typedef struct {
    objc_class *isa;
    id touches;
    double t;
} ui_event;
static void t_location(CGPoint *ret, ui_touch *self, SEL _c, id view) { *ret = self->loc; }
static void t_prevLocation(CGPoint *ret, ui_touch *self, SEL _c, id view) { *ret = self->prev; }
static int t_phase(ui_touch *self, SEL _c) { return self->phase; }
static int t_taps(ui_touch *self, SEL _c) { return self->taps; }
static double t_time(ui_touch *self, SEL _c) { return self->t; }
static id t_view(ui_touch *self, SEL _c) { return self->view; }
static id t_window(ui_touch *self, SEL _c) { return key_window; }
static const nmethod uitouch_i[] = {
    NM("locationInView:", t_location), NM("previousLocationInView:", t_prevLocation), NM("phase", t_phase),
    NM("tapCount", t_taps), NM("timestamp", t_time), NM("view", t_view), NM("window", t_window), {0}};
static id ev_allTouches(ui_event *self, SEL _c) { return self->touches; }
static id ev_touchesForView(ui_event *self, SEL _c, id v) { return self->touches; }
static double ev_time(ui_event *self, SEL _c) { return self->t; }
static const nmethod uievent_i[] = {
    NM("allTouches", ev_allTouches), NM("touchesForView:", ev_touchesForView), NM("touchesForWindow:", ev_touchesForView),
    NM("timestamp", ev_time), NM("type", ap_zero), {0}};

#define MAX_TOUCH 9 // 2 reales + 6 botones + dedo del STICK (input.c)
static ui_touch *live[MAX_TOUCH]; // por id de dedo (input.c)
static id touch_event;

// La vista que recibe los toques: la más alta (último addSubview) que contiene el punto (x,y en coordenadas
// de root). Las hijas directas de la ventana aceptan siempre (su frame puede venir apaisado o sin fijar).
static id hit_view(id root, float x, float y) {
    if (!root || VX(root)->hidden || !VX(root)->user_interaction)
        return NULL;
    int is_win = objc_is_kind(root, c_UIWindow);
    id subs = VX(root)->subviews;
    for (int i = ns_array_count(subs) - 1; i >= 0; i--) {
        id s = ns_array_get(subs, i);
        CGRect f = VX(s)->frame;
        float lx = x - f.o.x, ly = y - f.o.y;
        if (!is_win && (lx < 0 || ly < 0 || lx >= f.s.w || ly >= f.s.h))
            continue;
        id r = hit_view(s, is_win ? x : lx, is_win ? y : ly);
        if (r)
            return r;
    }
    return is_win ? NULL : root;
}

// input.c -> fase 0 began, 1 moved, 3 ended (coordenadas de la vista, ya giradas)
void ui_touch_event(int finger, int phase, float x, float y) {
    if (finger < 0 || finger >= MAX_TOUCH)
        return;
    id view = phase == 0 ? hit_view(key_window, x, y) : live[finger] ? live[finger]->view : NULL;
    static int nlog; // ponytail: diagnóstico corrida 015 (no pasa de la pantalla de sonido). Quitar al avanzar.
    if (phase != 1 && nlog++ < 300)
        l_info("toque dedo %d fase %d (%.0f, %.0f) -> %s", finger, phase, x, y,
               view ? class_getName(object_getClass(view)) : "SIN VISTA");
    if (!view)
        return;
    double now = sceKernelGetProcessTimeWide() / 1e6;
    ui_touch *t = live[finger];
    if (phase == 0) {
        if (t)
            objc_release(t);
        t = live[finger] = objc_alloc(c_UITouch);
        t->prev = (CGPoint){x, y};
        t->taps = 1;
        t->view = view;
    } else if (!t) {
        return;
    } else {
        t->prev = t->loc;
        if (x < 0) // dedo levantado (input.c no sabe dónde): iOS reporta el último punto, no (-1,-1)
            x = t->loc.x, y = t->loc.y;
    }
    t->loc = (CGPoint){x, y};
    t->phase = phase;
    t->t = now;
    id set = MSG1(objc_getClass("NSSet"), "setWithObject:", t);
    ui_event *ev = touch_event;
    objc_release(ev->touches);
    ev->touches = objc_retain(set);
    ev->t = now;
    static const char *sels[] = {"touchesBegan:withEvent:", "touchesMoved:withEvent:", "", "touchesEnded:withEvent:"};
    SEL ts = sel_registerName(sels[phase]);
    id target = t->view;
    while (target && !objc_responds(target, ts))
        target = VX(target)->superview;
    if (target)
        MSG2(target, sels[phase], set, ev);
    if (phase == 3) {
        objc_release(t);
        live[finger] = NULL;
    }
}

// ================================================================ UIImage + CoreGraphics

typedef struct {
    uint32_t magic; // 'CGIM'
    int w, h, refs, premul;
    uint8_t *rgba; // fila 0 = arriba
    GLuint tex;    // textura de la capa UIKit (ui_overlay_draw), creada al primer dibujo
} cg_image;
#define CGIM 0x4D494743
typedef struct {
    uint32_t magic; // 'CGCX'
    uint8_t *data;
    int w, h, bpc, bpr, info, gray, own;
    CGAffineTransform ctm;
    float fill[4];
} cg_ctx;
#define CGCX 0x58434743

static cg_image *img_new(int w, int h, uint8_t *rgba, int premul) {
    cg_image *i = calloc(1, sizeof(*i));
    *i = (cg_image){CGIM, w, h, 1, premul, rgba};
    return i;
}

static cg_image *img_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n);
    fread(d, 1, n, f);
    fclose(f);
    cg_image *img = NULL;
    int w, h, bpp;
    if (cgbi_is(d, n)) {
        uint8_t *px = cgbi_decode(d, n, &w, &h, &bpp, malloc);
        if (px)
            img = img_new(w, h, px, 1);
    } else {
        png_image p;
        memset(&p, 0, sizeof(p));
        p.version = PNG_IMAGE_VERSION;
        if (png_image_begin_read_from_memory(&p, d, n)) {
            p.format = PNG_FORMAT_RGBA;
            uint8_t *px = malloc(PNG_IMAGE_SIZE(p));
            if (png_image_finish_read(&p, NULL, px, 0, NULL))
                img = img_new(p.width, p.height, px, 0);
            else
                free(px);
        }
    }
    free(d);
    if (!img)
        l_error("UIImage: no se pudo decodificar %s (¿JPEG?)", path);
    return img;
}

typedef struct {
    objc_class *isa;
    cg_image *cg;
} ui_image;
static id uiimage_from(cg_image *cg) {
    if (!cg)
        return NULL;
    ui_image *u = objc_alloc(c_UIImage);
    u->cg = cg;
    return u;
}
static const char *find_image(const char *name, char *out, int max) {
    struct stat st;
    if (name[0] == 'u' && name[1] == 'x' && !stat(name, &st))
        return name;
    snprintf(out, max, BUNDLE_PATH "%s", name);
    if (!stat(out, &st))
        return out;
    if (!strchr(name, '.')) {
        snprintf(out, max, BUNDLE_PATH "%s.png", name);
        if (!stat(out, &st))
            return out;
    }
    return NULL;
}
static id im_imageNamed(id cls, SEL _c, id name) {
    char p[512];
    const char *path = find_image(ns_cstr(name), p, sizeof(p));
    id r = path ? uiimage_from(img_load(path)) : NULL;
    if (!r)
        l_error("UIImage imageNamed:%s -> nil", ns_cstr(name));
    return r ? AR(r) : NULL;
}
static id im_withContentsOfFile(id cls, SEL _c, id path) { return im_imageNamed(cls, _c, path); }
static id im_initWithContentsOfFile(id self, SEL _c, id path) {
    char p[512];
    const char *f = find_image(ns_cstr(path), p, sizeof(p));
    cg_image *cg = f ? img_load(f) : NULL;
    if (!cg) {
        objc_free(self);
        return NULL;
    }
    ((ui_image *)self)->cg = cg;
    return self;
}
static id im_imageWithCGImage(id cls, SEL _c, cg_image *cg) {
    if (cg)
        cg->refs++;
    return cg ? AR(uiimage_from(cg)) : NULL;
}
static cg_image *im_CGImage(ui_image *self, SEL _c) { return self->cg; }
static void im_size(CGSize *ret, ui_image *self, SEL _c) {
    *ret = self->cg ? (CGSize){self->cg->w, self->cg->h} : (CGSize){0, 0};
}
static void cg_image_release(cg_image *i);
static void im_dealloc(ui_image *self, SEL _c) {
    cg_image_release(self->cg);
    objc_free(self);
}
static const nmethod uiimage_c[] = {
    NM("imageNamed:", im_imageNamed), NM("imageWithContentsOfFile:", im_withContentsOfFile),
    NM("imageWithCGImage:", im_imageWithCGImage), {0}};
static const nmethod uiimage_i[] = {
    NM("initWithContentsOfFile:", im_initWithContentsOfFile), NM("CGImage", im_CGImage), NM("size", im_size),
    NM("scale", l_one), NM("dealloc", im_dealloc), NM("drawInRect:", v_noop), NM("drawAtPoint:", v_noop),
    NM("imageOrientation", ap_zero), {0}};

static int cg_GetWidth(cg_image *i) { return i && i->magic == CGIM ? i->w : 0; }
static int cg_GetHeight(cg_image *i) { return i && i->magic == CGIM ? i->h : 0; }
static int cg_Eight(cg_image *i) { return 8; }
static int cg_BitsPerPixel(cg_image *i) { return 32; }
static int cg_BytesPerRow(cg_image *i) { return i ? i->w * 4 : 0; }
static int cg_AlphaInfo(cg_image *i) { return 3; } // kCGImageAlphaLast
static void *cg_ColorSpace(cg_image *i) { return (void *)1; }
static cg_image *cg_ImageRetain(cg_image *i) {
    if (i && i->magic == CGIM)
        i->refs++;
    return i;
}
static void cg_image_release(cg_image *i) {
    if (i && i->magic == CGIM && --i->refs <= 0) {
        i->magic = 0;
        if (i->tex)
            glDeleteTextures(1, &i->tex);
        free(i->rgba);
        free(i);
    }
}
static cg_image *cg_ImageCreateWithMask(cg_image *img, cg_image *mask) { return cg_ImageRetain(img); }

static cg_ctx *cg_BitmapContextCreate(void *data, int w, int h, int bpc, int bpr, void *cs, int info) {
    cg_ctx *c = calloc(1, sizeof(*c));
    int gray = cs == (void *)2 || (info & 0x1F) == 7; // gris o kCGImageAlphaOnly
    if (!bpr)
        bpr = w * (gray ? 1 : 4);
    *c = (cg_ctx){CGCX, data, w, h, bpc, bpr, info, gray, 0, IDENTITY, {0, 0, 0, 1}};
    if (!data) {
        c->data = calloc(1, bpr * h);
        c->own = 1;
    }
    return c;
}
static void cg_ContextRelease(cg_ctx *c) {
    if (c && c->magic == CGCX) {
        c->magic = 0;
        if (c->own)
            free(c->data);
        free(c);
    }
}
static void *cg_ColorSpaceRGB(void) { return (void *)1; }
static void *cg_ColorSpaceGray(void) { return (void *)2; }
static void cg_noop(void) {}
static void *cg_BitmapGetData(cg_ctx *c) { return c->data; }
static void cg_TranslateCTM(cg_ctx *c, float tx, float ty) {
    c->ctm.tx += c->ctm.a * tx + c->ctm.c * ty;
    c->ctm.ty += c->ctm.b * tx + c->ctm.d * ty;
}
static void cg_ScaleCTM(cg_ctx *c, float sx, float sy) {
    c->ctm.a *= sx, c->ctm.b *= sx, c->ctm.c *= sy, c->ctm.d *= sy;
}
static void cg_RotateCTM(cg_ctx *c, float r) {
    CGAffineTransform m = c->ctm;
    float s = sinf(r), k = cosf(r);
    c->ctm.a = m.a * k + m.c * s, c->ctm.b = m.b * k + m.d * s;
    c->ctm.c = m.c * k - m.a * s, c->ctm.d = m.d * k - m.b * s;
}
static void put_px(cg_ctx *c, uint8_t *d, const uint8_t *s, int premul_src) {
    uint8_t r = s[0], g = s[1], b = s[2], a = s[3];
    int alpha_mode = c->info & 0x1F; // 1 premulLast 2 premulFirst 3 last 4 first 5 noneSkipLast 6 noneSkipFirst
    int want_premul = alpha_mode == 1 || alpha_mode == 2;
    if (want_premul && !premul_src)
        r = r * a / 255, g = g * a / 255, b = b * a / 255;
    else if (!want_premul && premul_src && a)
        r = r * 255 / a, g = g * 255 / a, b = b * 255 / a;
    if (c->gray) {
        d[0] = alpha_mode == 7 ? a : (r * 77 + g * 151 + b * 28) >> 8;
        return;
    }
    int little = (c->info & 0x7000) == 0x2000; // kCGBitmapByteOrder32Little
    if (alpha_mode == 2 || alpha_mode == 4 || alpha_mode == 6) { // ARGB lógico
        uint8_t v[4] = {a, r, g, b};
        if (little)
            d[0] = v[3], d[1] = v[2], d[2] = v[1], d[3] = v[0];
        else
            memcpy(d, v, 4);
    } else {
        uint8_t v[4] = {r, g, b, alpha_mode >= 5 ? 255 : a};
        if (little)
            d[0] = v[3], d[1] = v[2], d[2] = v[1], d[3] = v[0];
        else
            memcpy(d, v, 4);
    }
}
// CGContextDrawImage(ctx, CGRect rect, img): espacio de usuario con origen abajo-izquierda; la
// fila 0 de la memoria del bitmap es la de arriba (y = h-1). Muestreo al vecino más cercano.
static void cg_DrawImage(cg_ctx *c, CGRect r, cg_image *img) {
    if (!c || c->magic != CGCX || !img || img->magic != CGIM || r.s.w == 0 || r.s.h == 0)
        return;
    CGAffineTransform m = c->ctm;
    float det = m.a * m.d - m.b * m.c;
    if (det == 0)
        return;
    int bpp = c->gray ? 1 : 4;
    for (int row = 0; row < c->h; row++) {
        float dy = c->h - 1 - row + 0.5f;
        uint8_t *dst = c->data + row * c->bpr;
        for (int col = 0; col < c->w; col++) {
            float dx = col + 0.5f;
            // inversa del CTM
            float ux = (m.d * (dx - m.tx) - m.c * (dy - m.ty)) / det;
            float uy = (-m.b * (dx - m.tx) + m.a * (dy - m.ty)) / det;
            float u = (ux - r.o.x) / r.s.w, v = (uy - r.o.y) / r.s.h;
            if (u < 0 || u >= 1 || v < 0 || v >= 1)
                continue;
            int ix = (int)(u * img->w), iy = (int)((1 - v) * img->h);
            if (iy >= img->h)
                iy = img->h - 1;
            put_px(c, dst + col * bpp, img->rgba + (iy * img->w + ix) * 4, img->premul);
        }
    }
}
static void cg_ClearRect(cg_ctx *c, CGRect r) {
    if (c && c->magic == CGCX)
        memset(c->data, 0, c->bpr * c->h); // ponytail: limpia todo el contexto (los juegos limpian todo)
}
static void cg_SetRGBFill(cg_ctx *c, float r, float g, float b, float a) {
    c->fill[0] = r, c->fill[1] = g, c->fill[2] = b, c->fill[3] = a;
}
static void cg_FillRect(cg_ctx *c, CGRect r) {
    if (!c || c->magic != CGCX)
        return;
    uint8_t px[4] = {c->fill[0] * 255, c->fill[1] * 255, c->fill[2] * 255, c->fill[3] * 255};
    int x0 = r.o.x + c->ctm.tx, y0 = r.o.y + c->ctm.ty, x1 = x0 + r.s.w, y1 = y0 + r.s.h;
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < c->h; y++)
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < c->w; x++)
            put_px(c, c->data + (c->h - 1 - y) * c->bpr + x * (c->gray ? 1 : 4), px, 0);
}
// ---- para coretext.c (texto con FreeType): mapeo usuario -> memoria y píxel con cobertura (de Zenonia5-iOS-vita)
int cg_ctx_map(void *ctx, float ux, float uy, float *col, float *row) {
    cg_ctx *c = ctx;
    if (!c || c->magic != CGCX)
        return 0;
    CGAffineTransform m = c->ctm;
    *col = m.a * ux + m.c * uy + m.tx;
    *row = c->h - (m.b * ux + m.d * uy + m.ty); // fila 0 de la memoria = arriba
    return 1;
}
static int px_alpha(cg_ctx *c, const uint8_t *d) {
    if (c->gray)
        return d[0];
    int mode = c->info & 0x1F, little = (c->info & 0x7000) == 0x2000, first = mode == 2 || mode == 4 || mode == 6;
    return first != little ? d[0] : d[3];
}
// Cobertura 0-255 del color de relleno. ponytail: se queda con el mayor alfa en vez de componer "over".
void cg_ctx_cover(void *ctx, int col, int row, int cov) {
    cg_ctx *c = ctx;
    if (col < 0 || row < 0 || col >= c->w || row >= c->h || !cov)
        return;
    uint8_t *d = c->data + row * c->bpr + col * (c->gray ? 1 : 4);
    int a = c->fill[3] * cov;
    if (a <= px_alpha(c, d))
        return;
    uint8_t px[4] = {c->fill[0] * 255, c->fill[1] * 255, c->fill[2] * 255, a};
    put_px(c, d, px, 0);
}
static cg_image *cg_BitmapCreateImage(cg_ctx *c) {
    uint8_t *px = malloc(c->w * c->h * 4);
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            const uint8_t *s = c->data + y * c->bpr + x * (c->gray ? 1 : 4);
            uint8_t *d = px + (y * c->w + x) * 4;
            if (c->gray)
                d[0] = d[1] = d[2] = s[0], d[3] = 255;
            else
                memcpy(d, s, 4);
        }
    return img_new(c->w, c->h, px, (c->info & 0x1F) <= 2);
}
static void cg_AffineMakeRotation(CGAffineTransform *ret, float a) {
    *ret = (CGAffineTransform){cosf(a), sinf(a), -sinf(a), cosf(a), 0, 0};
}
static void cg_AffineMakeTranslation(CGAffineTransform *ret, float x, float y) {
    *ret = (CGAffineTransform){1, 0, 0, 1, x, y};
}
static void cg_AffineMakeScale(CGAffineTransform *ret, float x, float y) { *ret = (CGAffineTransform){x, 0, 0, y, 0, 0}; }
static void cg_AffineRotate(CGAffineTransform *ret, CGAffineTransform t, float a) {
    float s = sinf(a), k = cosf(a);
    *ret = (CGAffineTransform){t.a * k + t.c * s, t.b * k + t.d * s, t.c * k - t.a * s, t.d * k - t.b * s, t.tx, t.ty};
}
static void cg_RectOffset(CGRect *ret, CGRect r, float dx, float dy) {
    *ret = r;
    ret->o.x += dx, ret->o.y += dy;
}
static void cg_RectMake(CGRect *ret, float x, float y, float w, float h) { *ret = (CGRect){{x, y}, {w, h}}; }
static void *cg_dummy(void) { return (void *)1; }

// Geometría de CGRect (por valor en r0-r3, como en AAPCS)
static float cg_RectGetMinX(CGRect r) { return r.s.w < 0 ? r.o.x + r.s.w : r.o.x; }
static float cg_RectGetMinY(CGRect r) { return r.s.h < 0 ? r.o.y + r.s.h : r.o.y; }
static float cg_RectGetMaxY(CGRect r) { return cg_RectGetMinY(r) + fabsf(r.s.h); }
static float cg_RectGetMidX(CGRect r) { return r.o.x + r.s.w / 2; }
static float cg_RectGetMidY(CGRect r) { return r.o.y + r.s.h / 2; }
static float cg_RectGetWidth(CGRect r) { return fabsf(r.s.w); }
static float cg_RectGetHeight(CGRect r) { return fabsf(r.s.h); }
static int cg_RectContainsPoint(CGRect r, CGPoint p) {
    return p.x >= cg_RectGetMinX(r) && p.x < cg_RectGetMinX(r) + fabsf(r.s.w) && p.y >= cg_RectGetMinY(r) &&
           p.y < cg_RectGetMaxY(r);
}
static void cg_RectInset(CGRect *ret, CGRect r, float dx, float dy) {
    *ret = (CGRect){{r.o.x + dx, r.o.y + dy}, {r.s.w - 2 * dx, r.s.h - 2 * dy}};
}
static void cg_AffineScale(CGAffineTransform *ret, CGAffineTransform t, float sx, float sy) {
    *ret = (CGAffineTransform){t.a * sx, t.b * sx, t.c * sy, t.d * sy, t.tx, t.ty};
}
// CGDataProvider + CGImageCreate (capturas para compartir): copia a RGBA
typedef struct {
    const void *data;
    size_t size;
} cg_provider;
static cg_provider *cg_DataProviderCreateWithData(void *info, const void *data, size_t size, void *release) {
    cg_provider *p = malloc(sizeof(*p));
    *p = (cg_provider){data, size};
    return p;
}
static cg_image *cg_ImageCreate(int w, int h, int bpc, int bpp, int bpr, void *cs, int info, cg_provider *prov) {
    if (!prov || bpp != 32)
        return NULL;
    uint8_t *px = malloc(w * h * 4);
    for (int y = 0; y < h; y++)
        memcpy(px + y * w * 4, (const uint8_t *)prov->data + y * bpr, w * 4);
    return img_new(w, h, px, (info & 0x1F) <= 2);
}

// UIGraphics*ImageContext (renderizado de texto de UIKit: no hay fuentes de Apple; queda vacío)
static cg_ctx *ui_ctx;
static void ui_BeginImageContext(float w, float h) {
    ui_ctx = cg_BitmapContextCreate(NULL, (int)w, (int)h, 8, 0, (void *)1, 1);
}
static cg_ctx *ui_GetCurrentContext(void) { return ui_ctx; }
void *ui_current_ctx(void) { return ui_ctx; }
static cg_ctx *ui_stack[8];
static int ui_sp;
static void ui_PushContext(cg_ctx *c) {
    if (ui_sp < 8)
        ui_stack[ui_sp++] = ui_ctx;
    ui_ctx = c;
}
static void ui_PopContext(void) { ui_ctx = ui_sp ? ui_stack[--ui_sp] : NULL; }
static id ui_GetImage(void) { return ui_ctx ? AR(uiimage_from(cg_BitmapCreateImage(ui_ctx))) : NULL; }
static void ui_EndImageContext(void) {
    cg_ContextRelease(ui_ctx);
    ui_ctx = NULL;
}
static id ui_nil(void) { return NULL; }

// ================================================================ UIColor / UIFont / UIAlertView / controllers

static id ret_self(id self, SEL _c) { return self; }
static id ret_cls_obj(id cls, SEL _c) { return AR(objc_alloc((objc_class *)cls)); }
// UIColor = su propio CGColor (CGColor devuelve self); componentes RGBA reales para el texto de UILabel/FontLabel
typedef struct {
    objc_class *isa;
    float c[4];
} ui_color;
static id col_init(ui_color *self, SEL _c, float r, float g, float b, float a) {
    self->c[0] = r, self->c[1] = g, self->c[2] = b, self->c[3] = a;
    return self;
}
static id col_initWhite(ui_color *self, SEL _c, float w, float a) { return col_init(self, _c, w, w, w, a); }
static id col_new(objc_class *cls, float r, float g, float b, float a) {
    return AR(col_init(objc_alloc(cls), NULL, r, g, b, a));
}
static id col_rgba(objc_class *cls, SEL _c, float r, float g, float b, float a) { return col_new(cls, r, g, b, a); }
static id col_white(objc_class *cls, SEL _c, float w, float a) { return col_new(cls, w, w, w, a); }
#define COLOR(fn, r, g, b, a) \
    static id fn(objc_class *cls, SEL _c) { return col_new(cls, r, g, b, a); }
COLOR(col_black, 0, 0, 0, 1)
COLOR(col_whiteC, 1, 1, 1, 1)
COLOR(col_clear, 0, 0, 0, 0)
COLOR(col_gray, .5f, .5f, .5f, 1)
COLOR(col_dgray, 1 / 3.f, 1 / 3.f, 1 / 3.f, 1)
COLOR(col_lgray, 2 / 3.f, 2 / 3.f, 2 / 3.f, 1)
COLOR(col_red, 1, 0, 0, 1)
COLOR(col_green, 0, 1, 0, 1)
COLOR(col_blue, 0, 0, 1, 1)
COLOR(col_yellow, 1, 1, 0, 1)
COLOR(col_orange, 1, .5f, 0, 1)
static id col_withAlpha(ui_color *self, SEL _c, float a) {
    return col_new(self->isa, self->c[0], self->c[1], self->c[2], a);
}
void ui_color_fill(void *ctx, id color) {
    cg_ctx *c = ctx;
    ui_color *u = (ui_color *)color;
    if (c && c->magic == CGCX && u && objc_is_kind(color, c_UIColor))
        memcpy(c->fill, u->c, sizeof(c->fill));
}
static void col_setFill(id self, SEL _c) { ui_color_fill(ui_ctx, self); }
static const nmethod uicolor_c[] = {
    NM("colorWithRed:green:blue:alpha:", col_rgba), NM("blackColor", col_black), NM("whiteColor", col_whiteC),
    NM("clearColor", col_clear), NM("grayColor", col_gray), NM("darkGrayColor", col_dgray),
    NM("lightGrayColor", col_lgray), NM("redColor", col_red), NM("greenColor", col_green), NM("blueColor", col_blue),
    NM("yellowColor", col_yellow), NM("orangeColor", col_orange), NM("colorWithWhite:alpha:", col_white),
    NM("colorWithPatternImage:", col_whiteC), {0}};
static const nmethod uicolor_i[] = {
    NM("initWithRed:green:blue:alpha:", col_init), NM("initWithWhite:alpha:", col_initWhite), NM("CGColor", ret_self),
    NM("colorWithAlphaComponent:", col_withAlpha), NM("set", col_setFill), NM("setFill", col_setFill), {0}};
static const nmethod uifont_c[] = {
    NM("systemFontOfSize:", ret_cls_obj), NM("boldSystemFontOfSize:", ret_cls_obj), NM("fontWithName:size:", ret_cls_obj),
    NM("italicSystemFontOfSize:", ret_cls_obj), {0}};

typedef struct {
    objc_class *isa;
    id delegate, title, message;
    int nbuttons;
} alert;
static id al_init(alert *self, SEL _c, id title, id msg, id delegate, id cancel, id other, ...) {
    self->title = objc_retain(title);
    self->message = objc_retain(msg);
    self->delegate = delegate;
    self->nbuttons = (cancel != NULL) + (other != NULL);
    return self;
}
static id deferred_alert;
static void al_show(alert *self, SEL _c) {
    l_info("UIAlertView: \"%s\" / \"%s\" -> se cierra sola (botón 0)", ns_cstr(self->title), ns_cstr(self->message));
    objc_release(deferred_alert);
    deferred_alert = objc_retain(self); // se cierra en el próximo tick (fuera del código que la abrió)
}
static int al_addButton(alert *self, SEL _c, id t) { return self->nbuttons++; }
static int al_count(alert *self, SEL _c) { return self->nbuttons; }
static int al_cancelIndex(alert *self, SEL _c) { return 0; }
static void al_setDelegate(alert *self, SEL _c, id d) { self->delegate = d; }
static const nmethod uialert_i[] = {
    NM("initWithTitle:message:delegate:cancelButtonTitle:otherButtonTitles:", al_init), NM("show", al_show),
    NM("addButtonWithTitle:", al_addButton), NM("numberOfButtons", al_count), NM("cancelButtonIndex", al_cancelIndex),
    NM("setDelegate:", al_setDelegate), NM("dismissWithClickedButtonIndex:animated:", v_noop),
    NM("setTitle:", v_noop), NM("setMessage:", v_noop), NM("setCancelButtonIndex:", v_noop), {0}};

static void glk_controller_loaded(id vc);
static void set_outlet(id src, const char *label, id dst);
static id vc_init(id self, SEL _c) { return self; }
static id vc_initWithNib(id self, SEL _c, id nib, id bundle) {
    VX(self)->subviews = nib ? ns_string_new(ns_cstr(nib)) : NULL; // nombre del nib (subviews no se usa en VCs)
    return self;
}
// nibs/<nombre>.txt (tools/nib_prep.py): "view <Clase> w h" + outlets del File's Owner
static id vc_load_nib(id self) {
    const char *name = VX(self)->subviews ? ns_cstr(VX(self)->subviews) : class_getName(object_getClass(self));
    char p[256], line[256], a[96], b[96], c[96];
    float w, h;
    snprintf(p, sizeof(p), DATA_PATH "nibs/%s.txt", name);
    FILE *f = fopen(p, "r");
    if (!f)
        return NULL;
    id view = NULL;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "view %95s %f %f", a, &w, &h) == 3) {
            objc_class *k = objc_getClass(a);
            nib_coder *coder = objc_alloc(c_UINibDecoder);
            coder->frame = (CGRect){{0, 0}, {w, h}};
            view = MSG1(objc_alloc(k ? k : c_UIView), "initWithCoder:", coder);
            l_info("nib %s: vista %s %.0fx%.0f", name, a, w, h);
        } else if (sscanf(line, "outlet %95s %95s %95s", a, b, c) == 3 && !strcmp(a, "owner") && !strcmp(c, "view") &&
                   strcmp(b, "view")) {
            set_outlet(self, b, view);
        }
    }
    fclose(f);
    return view;
}
static id vc_view(id self, SEL _c) {
    view_ext *x = VX(self);
    if (!x->layer) {
        x->layer = objc_retain(vc_load_nib(self));
        if (!x->layer && objc_responds(self, sel_registerName("loadView")))
            MSG0(self, "loadView");
        if (!x->layer) {
            CGRect r = {{0, 0}, {320, 480}};
            x->layer = objc_alloc(c_UIView);
            v_initWithFrame(x->layer, NULL, r);
        }
        if (objc_responds(self, sel_registerName("viewDidLoad")))
            MSG0(self, "viewDidLoad");
        glk_controller_loaded(self);
    }
    return x->layer;
}
static void vc_setView(id self, SEL _c, id v) { VX(self)->layer = objc_retain(v); }
static void vc_appear(id vc);
static void vc_present(id self, SEL _c, id vc, int anim) {
    id v = (id)MSG0(vc, "view");
    if (key_window && v)
        v_addSubview(key_window, NULL, v);
    vc_appear(vc);
}
static void vc_dismiss(id self, SEL _c, int anim) {}
static const nmethod uivc_i[] = {
    NM("init", vc_init), NM("initWithNibName:bundle:", vc_initWithNib), NM("view", vc_view), NM("setView:", vc_setView),
    NM("loadView", v_noop), NM("viewDidLoad", v_noop), NM("presentModalViewController:animated:", vc_present),
    NM("dismissModalViewControllerAnimated:", vc_dismiss), NM("setWantsFullScreenLayout:", v_noop),
    NM("presentViewController:animated:completion:", vc_present), NM("dismissViewControllerAnimated:completion:", vc_dismiss),
    NM("viewWillAppear:", v_noop), NM("viewDidAppear:", v_noop), NM("viewWillDisappear:", v_noop),
    NM("viewDidDisappear:", v_noop), NM("supportedInterfaceOrientations", ap_zero), NM("shouldAutorotate", v_yes),
    NM("isViewLoaded", v_yes),
    NM("shouldAutorotateToInterfaceOrientation:", v_yes), NM("interfaceOrientation", ap_statusOrientation),
    NM("navigationController", ui_nil), NM("setTitle:", v_noop), NM("setModalTransitionStyle:", v_noop), {0}};

// ================================================================ UITextField + teclado (sceImeDialog)

typedef struct {
    objc_class *isa;
    id text, delegate, placeholder;
    int secure, maxlen;
} text_field;
static text_field *ime_field;
static int ime_open_now;
static uint16_t ime_title[64], ime_init[256], ime_buf[256];

static void utf8_to_16(const char *s, uint16_t *o, int max) {
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n < max - 1) {
        unsigned c = *p++;
        if (c >= 0xC0) {
            int k = c >= 0xE0 ? 2 : 1;
            c &= 0x3F >> k;
            while (k-- && *p)
                c = c << 6 | (*p++ & 0x3F);
        }
        o[n++] = c;
    }
    o[n] = 0;
}
int ime_active(void) { return ime_open_now; }
static void ime_start(text_field *f) {
    if (ime_open_now)
        return;
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    utf8_to_16("Texto", ime_title, 64);
    utf8_to_16(ns_cstr(f->text), ime_init, 256);
    p.supportedLanguages = 0;
    p.languagesForced = SCE_FALSE;
    p.type = SCE_IME_TYPE_DEFAULT;
    p.option = 0;
    p.textBoxMode = f->secure ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD : SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    p.title = ime_title;
    p.maxTextLength = f->maxlen > 0 && f->maxlen < 255 ? f->maxlen : 32;
    p.initialText = ime_init;
    p.inputTextBuffer = ime_buf;
    if (sceImeDialogInit(&p) >= 0) {
        ime_open_now = 1;
        ime_field = f;
        objc_retain(f);
    }
}
static void ime_poll(void) {
    if (!ime_open_now || sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return;
    SceImeDialogResult r;
    memset(&r, 0, sizeof(r));
    sceImeDialogGetResult(&r);
    sceImeDialogTerm();
    ime_open_now = 0;
    text_field *f = ime_field;
    ime_field = NULL;
    if (r.button == SCE_IME_DIALOG_BUTTON_ENTER) {
        char u8[768];
        int n = 0;
        for (int i = 0; ime_buf[i] && n < 760; i++) {
            unsigned c = ime_buf[i];
            if (c < 0x80)
                u8[n++] = c;
            else if (c < 0x800)
                u8[n++] = 0xC0 | c >> 6, u8[n++] = 0x80 | (c & 0x3F);
            else
                u8[n++] = 0xE0 | c >> 12, u8[n++] = 0x80 | ((c >> 6) & 0x3F), u8[n++] = 0x80 | (c & 0x3F);
        }
        u8[n] = 0;
        objc_release(f->text);
        f->text = ns_string_new(u8);
        l_info("teclado: \"%s\"", u8);
        ns_post_notification("UITextFieldTextDidChangeNotification", f, NULL);
    }
    if (f->delegate && objc_responds(f->delegate, sel_registerName("textFieldShouldReturn:")))
        MSG1(f->delegate, "textFieldShouldReturn:", f);
    if (f->delegate && objc_responds(f->delegate, sel_registerName("textFieldDidEndEditing:")))
        MSG1(f->delegate, "textFieldDidEndEditing:", f);
    objc_release(f);
}
static id tf_initWithFrame(text_field *self, SEL _c, CGRect r) {
    v_initWithFrame(self, _c, r);
    self->text = ns_string_new("");
    return self;
}
static id tf_text(text_field *self, SEL _c) { return self->text; }
static void tf_setText(text_field *self, SEL _c, id t) {
    objc_release(self->text);
    self->text = ns_string_new(ns_cstr(t));
}
static void tf_setDelegate(text_field *self, SEL _c, id d) { self->delegate = d; }
static id tf_delegate(text_field *self, SEL _c) { return self->delegate; }
static void tf_setSecure(text_field *self, SEL _c, int s) { self->secure = s & 0xFF; }
static int tf_become(text_field *self, SEL _c) {
    ime_start(self);
    return 1;
}
// Tocar el campo abre el teclado, como en iOS (si no, el toque se perdía: el campo tapa al EAGLView).
static void tf_touchesEnded(text_field *self, SEL _c, id touches, id ev) { ime_start(self); }
static const nmethod uitf_i[] = {
    NM("touchesBegan:withEvent:", v_noop), NM("touchesMoved:withEvent:", v_noop),
    NM("touchesEnded:withEvent:", tf_touchesEnded), NM("touchesCancelled:withEvent:", v_noop),
    NM("initWithFrame:", tf_initWithFrame), NM("text", tf_text), NM("setText:", tf_setText),
    NM("setDelegate:", tf_setDelegate), NM("delegate", tf_delegate), NM("setSecureTextEntry:", tf_setSecure),
    NM("becomeFirstResponder", tf_become), NM("setPlaceholder:", v_noop), NM("setFont:", v_noop),
    NM("setTextColor:", v_noop), NM("setBorderStyle:", v_noop), NM("setKeyboardType:", v_noop),
    NM("setReturnKeyType:", v_noop), NM("setAutocorrectionType:", v_noop), NM("setAutocapitalizationType:", v_noop),
    NM("setKeyboardAppearance:", v_noop), NM("setTextAlignment:", v_noop), NM("setClearButtonMode:", v_noop),
    NM("setEnablesReturnKeyAutomatically:", v_noop), NM("addTarget:action:forControlEvents:", v_noop), {0}};

// ================================================================ MediaPlayer (videos: se saltean)

static id movie_done; // MPMoviePlayerController que terminó (notificación diferida)
static uint64_t movie_done_at; // Gameloft marca isMoviePlaying 10 ms DESPUÉS de play: avisar el fin más tarde
static id mp_initWithURL(id self, SEL _c, id url) {
    l_info("MPMoviePlayerController %s (sin video: termina enseguida)", ns_cstr(url));
    return self;
}
static void mp_play(id self, SEL _c) {
    objc_release(movie_done);
    movie_done = objc_retain(self);
    movie_done_at = sceKernelGetProcessTimeWide() + 500000;
    l_info("MPMoviePlayerController play (fin en 0.5 s)");
}
static id mp_view(id self, SEL _c) { return vc_view(self, _c); }
static int mp_state(id self, SEL _c) { return 0; } // MPMusicPlaybackStateStopped: el juego pone su música
static id mp_player(id cls, SEL _c) { return AR(objc_alloc((objc_class *)cls)); }
static const nmethod mpmovie_i[] = {
    NM("initWithContentURL:", mp_initWithURL), NM("play", mp_play), NM("stop", v_noop), NM("pause", v_noop),
    NM("view", mp_view), NM("setScalingMode:", v_noop), NM("setMovieControlMode:", v_noop),
    NM("setControlStyle:", v_noop), NM("setFullscreen:", v_noop), NM("setFullscreen:animated:", v_noop),
    NM("setBackgroundColor:", v_noop), NM("setShouldAutoplay:", v_noop), NM("prepareToPlay", v_noop),
    NM("playbackState", mp_state), NM("setInitialPlaybackTime:", v_noop), {0}};
static const nmethod mpmusic_c[] = {
    NM("iPodMusicPlayer", mp_player), NM("applicationMusicPlayer", mp_player), {0}};
static const nmethod mpmusic_i[] = {
    NM("playbackState", mp_state), NM("stop", v_noop), NM("pause", v_noop), NM("play", v_noop),
    NM("nowPlayingItem", ui_nil), NM("setQueueWithItemCollection:", v_noop), NM("setQueueWithQuery:", v_noop),
    NM("beginGeneratingPlaybackNotifications", v_noop), NM("endGeneratingPlaybackNotifications", v_noop),
    NM("setShuffleMode:", v_noop), NM("setRepeatMode:", v_noop), NM("setVolume:", v_noop), {0}};


// ================================================================ GLKit (iOS 5) y CADisplayLink

extern uint32_t objc_call_imp6(void *imp, id self, SEL sel, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e,
                               uint32_t f);
extern void *objc_lookup_imp(id self, SEL sel);
static objc_class *c_GLKView, *c_GLKViewController, *c_CADisplayLink;
#ifndef PORT_GLK_SCALE
#define PORT_GLK_SCALE PORT_SCREEN_SCALE
#endif
typedef struct {
    id context, delegate;
    float scale;
    int color_fmt, depth_fmt, stencil_fmt, ms;
} glk_ext;
static glk_ext *GX(id v) { return (glk_ext *)((uint8_t *)VX(v) + sizeof(view_ext)); }
static int glk_w(id v) { return (int)(VX(v)->frame.s.w * GX(v)->scale + 0.5f); }
static int glk_h(id v) { return (int)(VX(v)->frame.s.h * GX(v)->scale + 0.5f); }
static void glk_setup(id self) { GX(self)->scale = PORT_GLK_SCALE; }
static id gv_initWithFrame(id self, SEL _c, CGRect f) {
    v_initWithFrame(self, _c, f);
    glk_setup(self);
    return self;
}
static id gv_initWithFrameContext(id self, SEL _c, CGRect f, id ctx) {
    gv_initWithFrame(self, _c, f);
    GX(self)->context = objc_retain(ctx);
    return self;
}
static id gv_initWithCoder(id self, SEL _c, nib_coder *coder) { return gv_initWithFrame(self, _c, coder->frame); }
static id gv_context(id self, SEL _c) { return GX(self)->context; }
static void gv_setContext(id self, SEL _c, id ctx) { GX(self)->context = objc_retain(ctx); }
static id gv_delegate(id self, SEL _c) { return GX(self)->delegate; }
static void gv_setDelegate(id self, SEL _c, id d) { GX(self)->delegate = d; }
static int gv_drawableWidth(id self, SEL _c) { return glk_w(self); }
static int gv_drawableHeight(id self, SEL _c) { return glk_h(self); }
static void gv_setFmt(id self, SEL _c, int v) {}
static int gv_fmt(id self, SEL _c) { return 0; }
static void gv_setScale(id self, SEL _c, float s) { GX(self)->scale = s; }
static float gv_scale(id self, SEL _c) { return GX(self)->scale; }
static void gv_bindDrawable(id self, SEL _c) {
    int w = glk_w(self), h = glk_h(self);
    if (w != fb_w || h != fb_h)
        l_info("GLKView drawable %dx%d", w, h);
    gles_set_size(w, h);
    gles_bind_screen();
    glViewport(0, 0, w, h);
}
static void gv_display(id self, SEL _c) {
    gv_bindDrawable(self, _c);
    id d = GX(self)->delegate;
    CGRect r = {{0, 0}, VX(self)->frame.s};
    union {
        float f;
        uint32_t u;
    } x = {r.o.x}, y = {r.o.y}, w = {r.s.w}, h = {r.s.h};
    SEL s = sel_registerName("glkView:drawInRect:");
    if (d && objc_responds(d, s))
        objc_call_imp6(objc_lookup_imp(d, s), d, s, (uint32_t)self, x.u, y.u, w.u, h.u, 0);
    else if (objc_responds(self, sel_registerName("drawRect:")))
        objc_call_imp6(objc_lookup_imp(self, sel_registerName("drawRect:")), self, sel_registerName("drawRect:"), x.u, y.u,
                       w.u, h.u, 0, 0);
    frames++;
    gles_present();
}
static id gv_snapshot(id self, SEL _c) { return NULL; }
static const nmethod glkview_i[] = {
    NM("initWithFrame:", gv_initWithFrame), NM("initWithFrame:context:", gv_initWithFrameContext),
    NM("initWithCoder:", gv_initWithCoder), NM("context", gv_context), NM("setContext:", gv_setContext),
    NM("delegate", gv_delegate), NM("setDelegate:", gv_setDelegate), NM("drawableWidth", gv_drawableWidth),
    NM("drawableHeight", gv_drawableHeight), NM("setDrawableColorFormat:", gv_setFmt),
    NM("setDrawableDepthFormat:", gv_setFmt), NM("setDrawableStencilFormat:", gv_setFmt),
    NM("setDrawableMultisample:", gv_setFmt), NM("drawableColorFormat", gv_fmt), NM("drawableDepthFormat", gv_fmt),
    NM("bindDrawable", gv_bindDrawable), NM("display", gv_display), NM("snapshot", gv_snapshot),
    NM("setEnableSetNeedsDisplay:", v_noop), NM("deleteDrawable", v_noop), NM("setContentScaleFactor:", gv_setScale),
    NM("contentScaleFactor", gv_scale), {0}};

// GLKViewController: un timer a preferredFramesPerSecond llama update/glkViewControllerUpdate: y display.
typedef struct {
    id timer, delegate;
    int fps, paused, frames;
    double last_update, last_draw, since_update, since_draw;
} glkvc_ext;
static glkvc_ext *GVX(id vc) { return (glkvc_ext *)((uint8_t *)VX(vc) + sizeof(view_ext)); }
static void gvc_tick(id self, SEL _c, id timer) {
    glkvc_ext *g = GVX(self);
    if (g->paused)
        return;
    double now = sceKernelGetProcessTimeWide() / 1e6;
    g->since_update = g->last_update ? now - g->last_update : 1.0 / g->fps;
    g->last_update = now;
    if (g->delegate && objc_responds(g->delegate, sel_registerName("glkViewControllerUpdate:")))
        MSG1(g->delegate, "glkViewControllerUpdate:", self);
    else if (objc_responds(self, sel_registerName("update")))
        MSG0(self, "update");
    id view = VX(self)->layer;
    if (view && objc_is_kind(view, c_GLKView)) {
        if (!GX(view)->delegate)
            GX(view)->delegate = self;
        g->since_draw = g->last_draw ? now - g->last_draw : 0;
        g->last_draw = now;
        gv_display(view, NULL);
        g->frames++;
    }
}
static void glk_start(id self) {
    glkvc_ext *g = GVX(self);
    if (g->timer)
        return;
    if (!g->fps)
        g->fps = 30; // valor por defecto de GLKViewController
    extern id ns_timer_schedule_c(double interval, id target, SEL sel);
    g->timer = ns_timer_schedule_c(1.0 / g->fps, self, sel_registerName("_mlGlkTick:"));
    l_info("GLKViewController %s: %d fps", class_getName(object_getClass(self)), g->fps);
}
static void glk_controller_loaded(id vc) {
    if (objc_is_kind(vc, c_GLKViewController))
        glk_start(vc);
}
static int gvc_fps(id self, SEL _c) { return GVX(self)->fps ? GVX(self)->fps : 30; }
static void gvc_setFps(id self, SEL _c, int f) {
    GVX(self)->fps = f > 0 ? (f > 60 ? 60 : f) : 30;
    l_info("GLKViewController preferredFramesPerSecond = %d", GVX(self)->fps);
}
static int gvc_paused(id self, SEL _c) { return GVX(self)->paused; }
static void gvc_setPaused(id self, SEL _c, int p) { GVX(self)->paused = p & 0xFF; }
static double gvc_sinceUpdate(id self, SEL _c) { return GVX(self)->since_update; }
static double gvc_sinceDraw(id self, SEL _c) { return GVX(self)->since_draw; }
static int gvc_frames(id self, SEL _c) { return GVX(self)->frames; }
static id gvc_delegate(id self, SEL _c) { return GVX(self)->delegate; }
static void gvc_setDelegate(id self, SEL _c, id d) { GVX(self)->delegate = d; }
static const nmethod glkvc_i[] = {
    NM("preferredFramesPerSecond", gvc_fps), NM("setPreferredFramesPerSecond:", gvc_setFps),
    NM("framesPerSecond", gvc_fps), NM("isPaused", gvc_paused), NM("paused", gvc_paused),
    NM("setPaused:", gvc_setPaused), NM("timeSinceLastUpdate", gvc_sinceUpdate), NM("timeSinceLastDraw", gvc_sinceDraw),
    NM("framesDisplayed", gvc_frames), NM("delegate", gvc_delegate), NM("setDelegate:", gvc_setDelegate),
    NM("setPauseOnWillResignActive:", v_noop), NM("setResumeOnDidBecomeActive:", v_noop), NM("_mlGlkTick:", gvc_tick),
    {0}};

// CADisplayLink = NSTimer a 60/frameInterval Hz que pasa el link como argumento.
typedef struct {
    objc_class *isa;
    id target, timer;
    SEL sel;
    int interval, paused;
    double ts;
} display_link;
static void dl_fire(display_link *self, SEL _c, id timer) {
    if (self->paused)
        return;
    self->ts = sceKernelGetProcessTimeWide() / 1e6;
    objc_msg(self->target, self->sel, (uint32_t)self, 0, 0, 0);
}
static id dl_make(id cls, SEL _c, id target, SEL sel) {
    display_link *d = objc_alloc(c_CADisplayLink);
    d->target = objc_retain(target);
    d->sel = sel_registerName(sel);
    d->interval = 1;
    return AR(d);
}
static void dl_add(display_link *self, SEL _c, id rl, id mode) {
    extern id ns_timer_schedule_c(double interval, id target, SEL sel);
    if (!self->timer)
        self->timer = ns_timer_schedule_c(self->interval / 60.0, self, sel_registerName("_mlFire:"));
    l_info("CADisplayLink -> -[%s %s] cada %d frame(s)", class_getName(object_getClass(self->target)), self->sel,
           self->interval);
}
static void dl_invalidate(display_link *self, SEL _c) {
    if (self->timer)
        MSG0(self->timer, "invalidate");
    self->timer = NULL;
}
static void dl_setInterval(display_link *self, SEL _c, int i) { self->interval = i > 0 ? i : 1; }
static int dl_interval(display_link *self, SEL _c) { return self->interval; }
static void dl_setPaused(display_link *self, SEL _c, int p) { self->paused = p & 0xFF; }
static int dl_paused(display_link *self, SEL _c) { return self->paused; }
static double dl_timestamp(display_link *self, SEL _c) { return self->ts; }
static double dl_duration(display_link *self, SEL _c) { return 1.0 / 60; }
static const nmethod cadl_c[] = {NM("displayLinkWithTarget:selector:", dl_make), {0}};
static const nmethod cadl_i[] = {
    NM("addToRunLoop:forMode:", dl_add), NM("removeFromRunLoop:forMode:", dl_invalidate), NM("invalidate", dl_invalidate),
    NM("setFrameInterval:", dl_setInterval), NM("frameInterval", dl_interval), NM("setPaused:", dl_setPaused),
    NM("isPaused", dl_paused), NM("paused", dl_paused), NM("timestamp", dl_timestamp), NM("duration", dl_duration),
    NM("_mlFire:", dl_fire), {0}};

// ================================================================ nib (tools/nib_prep.py -> nib.txt)

// layoutSubviews solo para vistas de la ventana visible (las de un nib oculto esperan, como en iOS).
static void call_layouts(void) {
    int keep = 0;
    for (int i = 0; i < nlayout; i++) {
        id v = layout_queue[i];
        if (!v)
            continue;
        id w = v_window(v, NULL);
        if (w && w == key_window)
            MSG0(v, "layoutSubviews");
        else
            layout_queue[keep++] = v;
    }
    nlayout = keep;
}

void set_outlet_impl(void);
static void set_outlet(id src, const char *label, id dst) {
    char setter[128];
    snprintf(setter, sizeof(setter), "set%c%s:", label[0] >= 'a' && label[0] <= 'z' ? label[0] - 32 : label[0], label + 1);
    SEL s = sel_registerName(setter);
    if (objc_responds(src, s))
        objc_msg(src, s, (uint32_t)dst, 0, 0, 0);
    else if (!objc_ivar_set(src, label, dst))
        l_error("nib: %s no tiene outlet '%s'", class_getName(object_getClass(src)), label);
}

// Formato: "delegate <Clase>", "window <w> <h>", "view <Clase> <w> <h>", "outlet <rol> <label> <rol>"
static void load_nib(void) {
    FILE *f = fopen(DATA_PATH "nib.txt", "r");
    if (!f) {
        l_info("sin nib.txt: el juego crea su ventana por código");
        return;
    }
    id roles[4] = {the_app, NULL, NULL, NULL}; // owner, delegate, window, view
    const char *names[4] = {"owner", "delegate", "window", "view"};
    char line[256], a[96], b[96], c[96];
    float w, h;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "delegate %95s", a) == 1) {
            objc_class *k = objc_getClass(a);
            if (k && !app_delegate)
                roles[1] = (id)MSG0(objc_alloc(k), "init");
            else
                roles[1] = app_delegate;
            l_info("nib: delegate %s%s", a, k ? "" : " (no existe: se usa el de UIApplicationMain)");
        } else if (sscanf(line, "window %f %f", &w, &h) == 2) {
            CGRect r = {{0, 0}, {w, h}};
            roles[2] = nib_window = objc_alloc(c_UIWindow);
            v_initWithFrame(roles[2], NULL, r);
        } else if (sscanf(line, "view %95s %f %f", a, &w, &h) == 3) {
            objc_class *k = objc_getClass(a);
            nib_coder *coder = objc_alloc(c_UINibDecoder);
            coder->frame = (CGRect){{0, 0}, {w, h}};
            roles[3] = (id)MSG1(objc_alloc(k ? k : c_UIView), "initWithCoder:", coder);
            if (roles[2] && roles[3])
                v_addSubview(roles[2], NULL, roles[3]);
            l_info("nib: vista %s %.0fx%.0f -> %p", a, w, h, roles[3]);
        } else if (sscanf(line, "outlet %95s %95s %95s", a, b, c) == 3) {
            id src = NULL, dst = NULL;
            for (int i = 0; i < 4; i++) {
                if (!strcmp(a, names[i]))
                    src = roles[i];
                if (!strcmp(c, names[i]))
                    dst = roles[i];
            }
            if (src == the_app && !strcmp(b, "delegate")) {
                if (!app_delegate)
                    app_delegate = dst;
            } else if (src && dst) {
                set_outlet(src, b, dst);
            }
        }
    }
    fclose(f);
    if (!app_delegate)
        app_delegate = roles[1];
}

// ================================================================ run loop

static int tick_depth;
static void overlay_tick(void); // capa UIKit (abajo)
volatile unsigned ml_alive;     // vueltas del run loop (y del menú de opciones): el watchdog distingue reposo de cuelgue
static volatile int wd_report; // el watchdog pide listar las vistas (se hace acá, en el hilo principal)
static void dump_views(id v, int depth) {
    if (!v || depth > 8)
        return;
    l_info("vistas: %*s%s%s", depth * 2, "", class_getName(object_getClass(v)), VX(v)->hidden ? " (oculta)" : "");
    id subs = VX(v)->subviews;
    for (int i = 0, n = subs ? ns_array_count(subs) : 0; i < n; i++)
        dump_views(ns_array_get(subs, i), depth + 1);
}

void ui_runloop_tick(double max_wait) {
    tick_depth++;
    ml_alive++;
    anims_tick();
    overlay_tick();
    if (wd_report) {
        wd_report = 0;
        dump_views(key_window, 0);
    }
    input_poll(); // -> ui_touch_event / botones
    ime_poll();
    accel_tick();
    if (nlayout)
        call_layouts();
    extern void gcd_main_tick(void);
    gcd_main_tick();
    if (deferred_alert) {
        id a = deferred_alert;
        deferred_alert = NULL;
        id d = ((alert *)a)->delegate;
        if (d && objc_responds(d, sel_registerName("alertView:clickedButtonAtIndex:")))
            MSG2(d, "alertView:clickedButtonAtIndex:", a, 0);
        if (d && objc_responds(d, sel_registerName("alertView:didDismissWithButtonIndex:")))
            MSG2(d, "alertView:didDismissWithButtonIndex:", a, 0);
        objc_release(a);
    }
    if (movie_done && sceKernelGetProcessTimeWide() >= movie_done_at) {
        id m = movie_done;
        movie_done = NULL;
        ns_post_notification("MPMoviePlayerPlaybackDidFinishNotification", m, NULL);
        objc_release(m);
    }
    double wait = tick_depth == 1 ? ns_run_timers() : 0; // timers no reentrantes
    if (wait > max_wait)
        wait = max_wait;
    if (wait > 0.0005)
        sceKernelDelayThread((SceUInt)(wait * 1e6));
    tick_depth--;
}

// Watchdog de diagnóstico en un hilo propio: si el hilo principal se traba DENTRO de código del juego (un timer
// que no vuelve), el bucle de abajo no corre y un watchdog ahí no salta (Real Soccer: congelado tras el saque
// inicial, sin dump). 8 s sin presentar tras el primer frame -> crash deliberado en ESTE hilo; el .psp2dmp trae
// PC/LR de todos los hilos (buscar el hilo principal). Quitar cuando no haga falta.
static int wd_thread(SceSize args, void *argp) {
    unsigned last = 0, last_alive = 0, reported = 0;
    uint64_t since = sceKernelGetProcessTimeWide();
    for (;;) {
        sceKernelDelayThread(500000);
        uint64_t now = sceKernelGetProcessTimeWide();
        if (frames != last || frames < 60) { // los primeros frames cargan mallas/texturas: más de 8 s es normal
            last = frames, since = now, last_alive = ml_alive;
        } else if (ml_alive != last_alive) {
            // run loop vivo sin presentar: el juego paró la animación (stopAnimation: pantalla UIKit, pausa). No es
            // un cuelgue; se avisa una vez por parada con las vistas de la ventana.
            last_alive = ml_alive, since = now;
            if (reported != frames) {
                reported = frames;
                l_info("[WATCHDOG] el juego dejó de presentar (frames=%u) con el run loop vivo: vistas de la ventana",
                       (unsigned)frames);
                wd_report = 1;
            }
        } else if (now - since > 8 * 1000000ULL) {
            l_error("[WATCHDOG] 8 s sin presentRenderbuffer (frames=%u%s): crash deliberado para el dump", (unsigned)frames,
                    in_present ? ", dentro de present" : "");
            sceKernelDelayThread(200000); // que llegue el log
            *(volatile int *)0 = 0;
        }
    }
    return 0;
}

void ui_runloop_forever(void) {
    static uint64_t last_log;
    l_info("run loop principal");
    SceUID wd = sceKernelCreateThread("ml_watchdog", wd_thread, 0x10000100, 0x4000, 0, 0, NULL);
    if (wd >= 0)
        sceKernelStartThread(wd, 0, NULL);
    for (;;) {
        objc_pool_push();
        ui_runloop_tick(0.02);
        objc_pool_pop();
        uint64_t now = sceKernelGetProcessTimeWide();
        if (now - last_log > 10 * 1000000ULL) {
            l_info("vivo: %u frames presentados, %u draw calls%s", (unsigned)frames, (unsigned)gles_get_draw_calls(),
                   in_present ? " (colgado DENTRO de presentRenderbuffer)" : "");
            last_log = now;
        }
    }
}

// int UIApplicationMain(argc, argv, NSString *principalClassName, NSString *delegateClassName)
static int f_UIApplicationMain(int argc, char **argv, id principal, id delegate_name) {
    l_info("UIApplicationMain(principal=%s, delegate=%s)", principal ? ns_cstr(principal) : "nil",
           delegate_name ? ns_cstr(delegate_name) : "nil");
    objc_pool_push();
    objc_class *pc = principal ? objc_getClass(ns_cstr(principal)) : NULL;
    if (pc && pc != c_UIApplication) {
        objc_free(the_app);
        the_app = (id)MSG0(objc_alloc(pc), "init");
    }
    if (delegate_name) {
        objc_class *dc = objc_getClass(ns_cstr(delegate_name));
        if (dc)
            app_delegate = (id)MSG0(objc_alloc(dc), "init");
        else
            l_error("UIApplicationMain: no existe la clase delegate %s", ns_cstr(delegate_name));
    }
    load_nib();
    if (!app_delegate)
        fatal_error("UIApplicationMain: sin delegate (ni por nombre ni en el nib)");
    l_info("delegate: %s", class_getName(object_getClass(app_delegate)));
    if (objc_responds(app_delegate, sel_registerName("application:didFinishLaunchingWithOptions:")))
        MSG2(app_delegate, "application:didFinishLaunchingWithOptions:", the_app, 0);
    else if (objc_responds(app_delegate, sel_registerName("applicationDidFinishLaunching:")))
        MSG1(app_delegate, "applicationDidFinishLaunching:", the_app);
    l_info("applicationDidFinishLaunching: volvió");
    if (!key_window && nib_window) { // iPhone OS 2: la ventana del nib "Visible at Launch"
        l_info("sin makeKeyAndVisible: se muestra la ventana del nib");
        w_makeKeyAndVisible(nib_window, NULL);
    }
    call_layouts();
    if (objc_responds(app_delegate, sel_registerName("applicationDidBecomeActive:")))
        MSG1(app_delegate, "applicationDidBecomeActive:", the_app);
    objc_pool_pop();
    ui_runloop_forever();
    return 0;
}

// ================================================================ CoreGraphics extra (Gangstar Rio / Six Guns)
// CoreText, CGFont y UIFont con texto real: coretext.c (FreeType + font.ttf).

static int cg_RectIsEmpty(CGRect r) { return r.s.w == 0 || r.s.h == 0 || isinf(r.o.x); }
static int cg_RectIsNull(CGRect r) { return isinf(r.o.x) || isinf(r.o.y); }
static float cg_RectGetMaxX(CGRect r) { return cg_RectGetMinX(r) + fabsf(r.s.w); }
static void cg_RectApplyAffine(CGRect *ret, CGRect r, CGAffineTransform t) {
    float xs[4] = {r.o.x, r.o.x + r.s.w, r.o.x, r.o.x + r.s.w}, ys[4] = {r.o.y, r.o.y, r.o.y + r.s.h, r.o.y + r.s.h};
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (int i = 0; i < 4; i++) {
        float x = t.a * xs[i] + t.c * ys[i] + t.tx, y = t.b * xs[i] + t.d * ys[i] + t.ty;
        x0 = fminf(x0, x), y0 = fminf(y0, y), x1 = fmaxf(x1, x), y1 = fmaxf(y1, y);
    }
    *ret = (CGRect){{x0, y0}, {x1 - x0, y1 - y0}};
}
static void cg_AffineConcat(CGAffineTransform *ret, CGAffineTransform t1, CGAffineTransform t2) {
    *ret = (CGAffineTransform){t1.a * t2.a + t1.b * t2.c, t1.a * t2.b + t1.b * t2.d, t1.c * t2.a + t1.d * t2.c,
                               t1.c * t2.b + t1.d * t2.d, t1.tx * t2.a + t1.ty * t2.c + t2.tx,
                               t1.tx * t2.b + t1.ty * t2.d + t2.ty};
}
static void cg_AffineTranslate(CGAffineTransform *ret, CGAffineTransform t, float tx, float ty) {
    *ret = t;
    ret->tx += t.a * tx + t.c * ty, ret->ty += t.b * tx + t.d * ty;
}
static const float white_rgba[4] = {1, 1, 1, 1};
static const float *cg_ColorGetComponents(id c) { // CGColor = el UIColor (o un dummy sin componentes)
    return c && (uintptr_t)c > 0x1000 && objc_is_kind(c, c_UIColor) ? ((ui_color *)c)->c : white_rgba;
}
static float cg_ColorGetAlpha(id c) { return cg_ColorGetComponents(c)[3]; }
static int cg_ColorSpaceGetModel(void *cs) { return cs == (void *)2 ? 0 : 1; } // gray / RGB
static void *cg_DataProviderFromCFData(id data) {
    return cg_DataProviderCreateWithData(NULL, MSG0(data, "bytes"), (size_t)MSG0(data, "length"), NULL);
}
static void ui_BeginImageContextOpts(float w, float h, int opaque, float scale) { ui_BeginImageContext(w, h); }
static void ui_RectFill(CGRect r) { cg_FillRect(ui_GetCurrentContext(), r); }

// CATransform3D (16 floats, m11..m44 por filas como en Apple: v' = v * M)
typedef struct {
    float m[16];
} CATransform3D;
static void ca_mul(CATransform3D *r, const CATransform3D *a, const CATransform3D *b) { // r = a * b
    CATransform3D o;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            o.m[i * 4 + j] = a->m[i * 4] * b->m[j] + a->m[i * 4 + 1] * b->m[4 + j] + a->m[i * 4 + 2] * b->m[8 + j] +
                             a->m[i * 4 + 3] * b->m[12 + j];
    *r = o;
}
static void ca_MakeRotation(CATransform3D *ret, float a, float x, float y, float z) {
    float l = sqrtf(x * x + y * y + z * z), s = sinf(a), c = cosf(a), k = 1 - c;
    if (l == 0) {
        *ret = (CATransform3D){{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
        return;
    }
    x /= l, y /= l, z /= l;
    *ret = (CATransform3D){{x * x * k + c, x * y * k + z * s, x * z * k - y * s, 0, y * x * k - z * s, y * y * k + c,
                            y * z * k + x * s, 0, z * x * k + y * s, z * y * k - x * s, z * z * k + c, 0, 0, 0, 0, 1}};
}
static void ca_Rotate(CATransform3D *ret, CATransform3D t, float a, float x, float y, float z) {
    CATransform3D r;
    ca_MakeRotation(&r, a, x, y, z);
    ca_mul(ret, &r, &t);
}
static void ca_Scale(CATransform3D *ret, CATransform3D t, float x, float y, float z) {
    CATransform3D s = {{x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1}};
    ca_mul(ret, &s, &t);
}
static void ca_Translate(CATransform3D *ret, CATransform3D t, float x, float y, float z) {
    CATransform3D s = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1}};
    ca_mul(ret, &s, &t);
}
// CMTime {int64 value; int32 timescale; uint32 flags; int64 epoch}: value llega sin alinear en r1:r2
typedef struct __attribute__((packed)) {
    int64_t value;
    int32_t timescale;
    uint32_t flags;
    int64_t epoch;
} CMTime;
static void cm_TimeMake(CMTime *ret, uint32_t lo, uint32_t hi, int32_t scale) {
    *ret = (CMTime){((int64_t)hi << 32) | lo, scale, 1, 0};
}
static CMTime k_kCMTimeZero = {0, 0, 1, 0};

static CGRect k_CGRectNull = {{INFINITY, INFINITY}, {0, 0}};
static float k_UIEdgeInsetsZero[4], k_UIWindowLevelStatusBar = 1000.0f;
static double k_kCLLocationAccuracyBest = -1.0, k_kCLLocationAccuracyKilometer = 1000.0;

// ================================================================ tabla / init

static CGPoint k_CGPointZero;
static CGRect k_CGRectZero;
static CGSize k_CGSizeZero;
static CGAffineTransform k_CGAffineTransformIdentity = {1, 0, 0, 1, 0, 0};
static id k_kEAGLColorFormatRGB565, k_kEAGLColorFormatRGBA8, k_kEAGLDrawablePropertyColorFormat,
    k_kEAGLDrawablePropertyRetainedBacking, k_MPMoviePlayerPlaybackDidFinishNotification,
    k_UITextFieldTextDidChangeNotification, k_UIApplicationLaunchOptionsRemoteNotificationKey, k_MPMediaItemPropertyTitle,
    k_MPMediaItemPropertyArtist, k_MPMediaItemPropertyAlbumTitle, k_MPMediaItemPropertyPersistentID,
    k_MPMediaPlaylistPropertyName, k_UIKeyboardWillShowNotification, k_UIKeyboardWillHideNotification,
    k_UIDeviceOrientationDidChangeNotification, k_UIApplicationDidEnterBackgroundNotification,
    k_UIApplicationWillEnterForegroundNotification, k_MPMusicPlayerControllerPlaybackStateDidChangeNotification,
    k_MPMoviePlayerContentPreloadDidFinishNotification, k_UIKeyboardFrameEndUserInfoKey,
    k_MPMusicPlayerControllerNowPlayingItemDidChangeNotification, k_MPMediaLibraryDidChangeNotification;

const import_entry uikit_imports[] = {
    F(UIApplicationMain, f_UIApplicationMain),
    F(CGRectIsEmpty, cg_RectIsEmpty), F(CGRectIsNull, cg_RectIsNull), F(CGRectGetMaxX, cg_RectGetMaxX),
    F(CGRectApplyAffineTransform, cg_RectApplyAffine), F(CGAffineTransformConcat, cg_AffineConcat),
    F(CGAffineTransformTranslate, cg_AffineTranslate), F(CGColorGetComponents, cg_ColorGetComponents),
    F(CGColorGetAlpha, cg_ColorGetAlpha), F(CGColorGetColorSpace, cg_ColorSpaceRGB),
    F(CGColorSpaceGetModel, cg_ColorSpaceGetModel), F(CGDataProviderCreateWithCFData, cg_DataProviderFromCFData),
    F(CGContextAddArc, cg_noop), F(CGContextAddLineToPoint, cg_noop), F(CGContextAddPath, cg_noop),
    F(CGContextClip, cg_noop), F(CGContextDrawRadialGradient, cg_noop), F(CGContextFillEllipseInRect, cg_noop),
    F(CGContextSetFillColorWithColor, ui_color_fill), F(CGContextSetRGBStrokeColor, cg_noop),
    F(CGContextStrokeEllipseInRect, cg_noop), F(CGContextStrokePath, cg_noop), F(CGContextSetFont, cg_noop),
    F(CGContextSetFontSize, cg_noop), F(CGGradientCreateWithColors, cg_dummy),
    F(UIGraphicsBeginImageContextWithOptions, ui_BeginImageContextOpts), F(UIRectFill, ui_RectFill),
    F(CATransform3DMakeRotation, ca_MakeRotation), F(CATransform3DRotate, ca_Rotate), F(CATransform3DScale, ca_Scale),
    F(CATransform3DTranslate, ca_Translate), F(CMTimeMake, cm_TimeMake), V(kCMTimeZero, &k_kCMTimeZero),
V(CGRectNull, &k_CGRectNull), V(UIEdgeInsetsZero, k_UIEdgeInsetsZero),
    V(UIWindowLevelStatusBar, &k_UIWindowLevelStatusBar), V(kCLLocationAccuracyBest, &k_kCLLocationAccuracyBest),
    V(kCLLocationAccuracyKilometer, &k_kCLLocationAccuracyKilometer),
    F(CGImageGetWidth, cg_GetWidth), F(CGImageGetHeight, cg_GetHeight), F(CGImageGetBitsPerComponent, cg_Eight),
    F(CGImageGetBitsPerPixel, cg_BitsPerPixel), F(CGImageGetBytesPerRow, cg_BytesPerRow),
    F(CGImageGetAlphaInfo, cg_AlphaInfo), F(CGImageGetColorSpace, cg_ColorSpace), F(CGImageRetain, cg_ImageRetain),
    F(CGImageRelease, cg_image_release), F(CGImageCreateWithMask, cg_ImageCreateWithMask),
    F(CGBitmapContextCreate, cg_BitmapContextCreate), F(CGBitmapContextGetData, cg_BitmapGetData),
    F(CGBitmapContextCreateImage, cg_BitmapCreateImage), F(CGContextRelease, cg_ContextRelease),
    F(CGContextDrawImage, cg_DrawImage), F(CGContextClearRect, cg_ClearRect), F(CGContextTranslateCTM, cg_TranslateCTM),
    F(CGContextScaleCTM, cg_ScaleCTM), F(CGContextRotateCTM, cg_RotateCTM), F(CGContextSetRGBFillColor, cg_SetRGBFill),
    F(CGContextFillRect, cg_FillRect), F(CGContextSetBlendMode, cg_noop), F(CGContextSetInterpolationQuality, cg_noop),
    F(CGContextSetShouldAntialias, cg_noop), F(CGContextSetAllowsAntialiasing, cg_noop),
    F(CGContextSaveGState, cg_noop), F(CGContextRestoreGState, cg_noop), F(CGContextSetGrayFillColor, cg_noop),
    F(CGContextSetTextDrawingMode, cg_noop), F(CGContextSetAlpha, cg_noop), F(CGContextDrawLinearGradient, cg_noop),
    F(CGColorSpaceCreateDeviceRGB, cg_ColorSpaceRGB), F(CGColorSpaceCreateDeviceGray, cg_ColorSpaceGray),
    F(CGColorSpaceRelease, cg_noop), F(CGGradientCreateWithColorComponents, cg_dummy), F(CGGradientRelease, cg_noop),
    F(CGAffineTransformMakeRotation, cg_AffineMakeRotation), F(CGAffineTransformMakeTranslation, cg_AffineMakeTranslation),
    F(CGAffineTransformMakeScale, cg_AffineMakeScale), F(CGAffineTransformRotate, cg_AffineRotate),
    F(CGRectOffset, cg_RectOffset), F(CGRectMake, cg_RectMake), F(CGDataProviderRelease, free),
    F(CGRectGetMinX, cg_RectGetMinX), F(CGRectGetMinY, cg_RectGetMinY), F(CGRectGetMaxY, cg_RectGetMaxY),
    F(CGRectGetMidX, cg_RectGetMidX), F(CGRectGetMidY, cg_RectGetMidY), F(CGRectGetWidth, cg_RectGetWidth),
    F(CGRectGetHeight, cg_RectGetHeight), F(CGRectContainsPoint, cg_RectContainsPoint), F(CGRectInset, cg_RectInset),
    F(CGAffineTransformScale, cg_AffineScale), F(CGDataProviderCreateWithData, cg_DataProviderCreateWithData),
    F(CGImageCreate, cg_ImageCreate), F(CGColorCreate, cg_dummy), F(CGColorRelease, cg_noop),
    F(CGContextAddArcToPoint, cg_noop), F(CGContextAddRect, cg_noop), F(CGContextBeginPath, cg_noop),
    F(CGContextClosePath, cg_noop), F(CGContextFillPath, cg_noop), F(CGContextMoveToPoint, cg_noop),
    F(CGContextSetFillColor, cg_noop), F(CGContextSetLineWidth, cg_noop), F(CGContextSetShadowWithColor, cg_noop),
    F(CGContextSetStrokeColor, cg_noop), F(CGContextSetStrokeColorSpace, cg_noop),
    F(CGContextStrokeLineSegments, cg_noop),
    V(CGPointZero, &k_CGPointZero), V(CGRectZero, &k_CGRectZero), V(CGSizeZero, &k_CGSizeZero),
    V(CGAffineTransformIdentity, &k_CGAffineTransformIdentity),
    F(UIGraphicsBeginImageContext, ui_BeginImageContext), F(UIGraphicsGetCurrentContext, ui_GetCurrentContext),
    F(UIGraphicsGetImageFromCurrentImageContext, ui_GetImage), F(UIGraphicsEndImageContext, ui_EndImageContext),
    F(UIGraphicsPushContext, ui_PushContext), F(UIGraphicsPopContext, ui_PopContext), F(UIImagePNGRepresentation, ui_nil),
    F(UIImageJPEGRepresentation, ui_nil), F(UIImageWriteToSavedPhotosAlbum, cg_noop),
    V(kEAGLColorFormatRGB565, &k_kEAGLColorFormatRGB565), V(kEAGLColorFormatRGBA8, &k_kEAGLColorFormatRGBA8),
    V(kEAGLDrawablePropertyColorFormat, &k_kEAGLDrawablePropertyColorFormat),
    V(kEAGLDrawablePropertyRetainedBacking, &k_kEAGLDrawablePropertyRetainedBacking),
    V(MPMoviePlayerPlaybackDidFinishNotification, &k_MPMoviePlayerPlaybackDidFinishNotification),
    V(MPMoviePlayerContentPreloadDidFinishNotification, &k_MPMoviePlayerContentPreloadDidFinishNotification),
    V(UITextFieldTextDidChangeNotification, &k_UITextFieldTextDidChangeNotification),
    V(UIApplicationLaunchOptionsRemoteNotificationKey, &k_UIApplicationLaunchOptionsRemoteNotificationKey),
    V(MPMediaItemPropertyTitle, &k_MPMediaItemPropertyTitle), V(MPMediaItemPropertyArtist, &k_MPMediaItemPropertyArtist),
    V(MPMediaItemPropertyAlbumTitle, &k_MPMediaItemPropertyAlbumTitle),
    V(MPMediaItemPropertyPersistentID, &k_MPMediaItemPropertyPersistentID),
    V(MPMediaPlaylistPropertyName, &k_MPMediaPlaylistPropertyName),
    V(UIKeyboardWillShowNotification, &k_UIKeyboardWillShowNotification),
    V(UIKeyboardWillHideNotification, &k_UIKeyboardWillHideNotification),
    V(UIKeyboardFrameEndUserInfoKey, &k_UIKeyboardFrameEndUserInfoKey),
    V(UIDeviceOrientationDidChangeNotification, &k_UIDeviceOrientationDidChangeNotification),
    V(UIApplicationDidEnterBackgroundNotification, &k_UIApplicationDidEnterBackgroundNotification),
    V(UIApplicationWillEnterForegroundNotification, &k_UIApplicationWillEnterForegroundNotification),
    V(MPMusicPlayerControllerPlaybackStateDidChangeNotification, &k_MPMusicPlayerControllerPlaybackStateDidChangeNotification),
    V(MPMusicPlayerControllerNowPlayingItemDidChangeNotification, &k_MPMusicPlayerControllerNowPlayingItemDidChangeNotification),
    V(MPMediaLibraryDidChangeNotification, &k_MPMediaLibraryDidChangeNotification),
};
TABLE_END(uikit)

// ================================================================ UIImageView / UIButton / animaciones UIView + capa UIKit
// Temple Run arma el menú principal con vistas UIKit (displayMainMenuOverlay) y para su animación GL mientras tanto.
// Las vistas con imagen se dibujan encima del FBO del juego en cada present (ui_overlay_draw, desde gles.c), y
// ui_runloop_tick re-presenta solo cuando el juego dejó de hacerlo.
// ponytail: sin transform, sin backgroundColor ni texto (UILabel/títulos de UIButton); agregar si una pantalla lo usa.
typedef struct {
    id image, bg, anim; // UIImageView: image/animationImages; UIButton: image/backgroundImage (estado normal)
    float anim_dur;
    int animating;
    double anim_t0;
    id target;
    SEL action;
    uint32_t events;
} img_ext;
static img_ext *IX(id v) { return (img_ext *)((uint8_t *)VX(v) + sizeof(view_ext)); }
static objc_class *c_UIImageView, *c_UIButton, *c_UILabel;
static void set_obj(id *slot, id v) {
    objc_retain(v);
    objc_release(*slot);
    *slot = v;
}

static id iv_initWithFrame(id self, SEL _c, CGRect f) {
    v_initWithFrame(self, _c, f);
    VX(self)->user_interaction = 0; // como UIKit: un UIImageView no se come los toques
    return self;
}
static id iv_initWithImage(id self, SEL _c, ui_image *img) {
    CGRect f = {{0, 0}, {img && img->cg ? img->cg->w : 0, img && img->cg ? img->cg->h : 0}};
    iv_initWithFrame(self, _c, f);
    set_obj(&IX(self)->image, (id)img);
    return self;
}
static id iv_image(id self, SEL _c) { return IX(self)->image; }
static void iv_setImage(id self, SEL _c, id img) { set_obj(&IX(self)->image, img); }
static void iv_setAnimationImages(id self, SEL _c, id a) { set_obj(&IX(self)->anim, a); }
static id iv_animationImages(id self, SEL _c) { return IX(self)->anim; }
static void iv_setAnimationDuration(id self, SEL _c, double d) { IX(self)->anim_dur = d; }
static void iv_startAnimating(id self, SEL _c) { IX(self)->animating = 1, IX(self)->anim_t0 = now_s(); }
static void iv_stopAnimating(id self, SEL _c) { IX(self)->animating = 0; }
static int iv_isAnimating(id self, SEL _c) { return IX(self)->animating; }
static void iv_dealloc(id self, SEL _c) {
    img_ext *x = IX(self);
    objc_release(x->image), objc_release(x->bg), objc_release(x->anim);
    objc_free(self); // UIView no tiene dealloc propio: el de NSObject también es objc_free
}
static const nmethod uiimageview_i[] = {
    NM("initWithFrame:", iv_initWithFrame), NM("initWithImage:", iv_initWithImage), NM("image", iv_image),
    NM("setImage:", iv_setImage), NM("setHighlightedImage:", v_noop), NM("setAnimationImages:", iv_setAnimationImages),
    NM("animationImages", iv_animationImages), NM("setAnimationDuration:", iv_setAnimationDuration),
    NM("setAnimationRepeatCount:", v_noop), NM("startAnimating", iv_startAnimating),
    NM("stopAnimating", iv_stopAnimating), NM("isAnimating", iv_isAnimating), NM("dealloc", iv_dealloc), {0}};

static id bt_buttonWithType(objc_class *cls, SEL _c, int type) { return AR(v_init(objc_alloc(cls), _c)); }
static void bt_setImage(id self, SEL _c, id img, uint32_t state) {
    if (!state)
        set_obj(&IX(self)->image, img);
}
static void bt_setBackgroundImage(id self, SEL _c, id img, uint32_t state) {
    if (!state)
        set_obj(&IX(self)->bg, img);
}
static id bt_imageForState(id self, SEL _c, uint32_t state) { return IX(self)->image; }
static id bt_bgForState(id self, SEL _c, uint32_t state) { return IX(self)->bg; }
// ponytail: un solo target/acción por control (lo que usan estos menús); lista si un juego registra varios.
static void bt_addTarget(id self, SEL _c, id target, SEL action, uint32_t events) {
    img_ext *x = IX(self);
    x->target = target, x->action = action, x->events = events;
}
static void bt_fire(id self, uint32_t ev) {
    img_ext *x = IX(self);
    if (x->action && (x->events & ev)) {
        l_info("UIButton -> -[%s %s]", x->target ? class_getName(object_getClass(x->target)) : "nil", x->action);
        objc_msg(x->target, x->action, (uint32_t)self, (uint32_t)touch_event, 0, 0);
    }
}
static void bt_touchesBegan(id self, SEL _c, id touches, id ev) { bt_fire(self, 1 << 0); } // TouchDown
static void bt_touchesEnded(id self, SEL _c, id touches, id ev) { bt_fire(self, 1 << 6); } // TouchUpInside
static const nmethod uibutton_c[] = {NM("buttonWithType:", bt_buttonWithType), {0}};
static const nmethod uibutton_i[] = {
    NM("setImage:forState:", bt_setImage), NM("setBackgroundImage:forState:", bt_setBackgroundImage),
    NM("imageForState:", bt_imageForState), NM("backgroundImageForState:", bt_bgForState),
    NM("currentImage", iv_image), NM("addTarget:action:forControlEvents:", bt_addTarget),
    NM("removeTarget:action:forControlEvents:", v_noop), NM("setTitle:forState:", v_noop),
    NM("setTitleColor:forState:", v_noop), NM("setEnabled:", v_noop), NM("isEnabled", v_yes),
    NM("setSelected:", v_noop), NM("setHighlighted:", v_noop), NM("setShowsTouchWhenHighlighted:", v_noop),
    NM("setAdjustsImageWhenHighlighted:", v_noop), NM("setContentMode:", v_noop),
    NM("touchesBegan:withEvent:", bt_touchesBegan), NM("touchesMoved:withEvent:", v_noop),
    NM("touchesEnded:withEvent:", bt_touchesEnded), NM("touchesCancelled:withEvent:", v_noop),
    NM("dealloc", iv_dealloc), {0}};

// ---- UILabel: el juego usa FontLabel (subclase con ZFont/CGFont); su drawTextInRect: dibuja con
// CGContextShowGlyphsAtPoint (coretext.c) sobre el contexto que la capa empuja al cachear el texto.
typedef struct {
    id text, color, font;
    int align, lines, lbm, highlighted, dirty;
    cg_image *cache;
    float cw, ch;
} label_ext;
static label_ext *LX(id v) { return (label_ext *)((uint8_t *)VX(v) + sizeof(view_ext)); }
static id lb_initWithFrame(id self, SEL _c, CGRect f) {
    v_initWithFrame(self, _c, f);
    VX(self)->user_interaction = 0;
    label_ext *e = LX(self);
    e->lines = 1, e->dirty = 1;
    set_obj(&e->color, col_new(c_UIColor, 0, 0, 0, 1));
    return self;
}
#define LB_OBJ(get, set, field)                                                                                        \
    static id get(id self, SEL _c) { return LX(self)->field; }                                                         \
    static void set(id self, SEL _c, id v) { set_obj(&LX(self)->field, v), LX(self)->dirty = 1; }
#define LB_INT(get, set, field)                                                                                        \
    static int get(id self, SEL _c) { return LX(self)->field; }                                                        \
    static void set(id self, SEL _c, int v) { LX(self)->field = v, LX(self)->dirty = 1; }
LB_OBJ(lb_text, lb_setText, text)
LB_OBJ(lb_color, lb_setColor, color)
LB_OBJ(lb_font, lb_setFont, font)
LB_INT(lb_align, lb_setAlign, align)
LB_INT(lb_lines, lb_setLines, lines)
LB_INT(lb_lbm, lb_setLbm, lbm)
LB_INT(lb_highlighted, lb_setHighlighted, highlighted)
static id lb_nil(id self, SEL _c) { return NULL; }
static float lb_zero_f(id self, SEL _c) { return 0; }
void ct_draw_text(void *ctx, const char *s, float px, float x, float y, float w, float h, int align); // coretext.c
extern int coretext_ok;
// UILabel normal (sin ZFont): UIFont de coretext.c = {isa, float size}
static void lb_drawTextInRect(id self, SEL _c, CGRect r) {
    label_ext *e = LX(self);
    if (!coretext_ok || !e->text)
        return;
    ui_color_fill(ui_ctx, e->color);
    float px = e->font ? ((float *)e->font)[1] : 17;
    ct_draw_text(ui_ctx, ns_cstr(e->text), px > 0 ? px : 17, r.o.x, r.o.y, r.s.w, r.s.h, e->align);
}
static void lb_dealloc(id self, SEL _c) {
    label_ext *e = LX(self);
    set_obj(&e->text, NULL), set_obj(&e->color, NULL), set_obj(&e->font, NULL);
    if (e->cache)
        cg_image_release(e->cache);
    objc_free(self);
}
static const nmethod uilabel_i[] = {
    NM("initWithFrame:", lb_initWithFrame), NM("text", lb_text), NM("setText:", lb_setText),
    NM("textColor", lb_color), NM("setTextColor:", lb_setColor), NM("font", lb_font), NM("setFont:", lb_setFont),
    NM("textAlignment", lb_align), NM("setTextAlignment:", lb_setAlign), NM("numberOfLines", lb_lines),
    NM("setNumberOfLines:", lb_setLines), NM("lineBreakMode", lb_lbm), NM("setLineBreakMode:", lb_setLbm),
    NM("isHighlighted", lb_highlighted), NM("highlighted", lb_highlighted), NM("setHighlighted:", lb_setHighlighted),
    NM("highlightedTextColor", lb_nil), NM("setHighlightedTextColor:", v_noop), NM("shadowColor", lb_nil),
    NM("setShadowColor:", v_noop), NM("setShadowOffset:", v_noop), NM("isEnabled", v_yes), NM("setEnabled:", v_noop),
    NM("adjustsFontSizeToFitWidth", v_no), NM("setAdjustsFontSizeToFitWidth:", v_noop),
    NM("minimumFontSize", lb_zero_f), NM("setMinimumFontSize:", v_noop), NM("baselineAdjustment", v_no),
    NM("setBaselineAdjustment:", v_noop), NM("sizeToFit", v_noop), NM("drawTextInRect:", lb_drawTextInRect),
    NM("dealloc", lb_dealloc), {0}};

// ---- dibujo: proyección de pantalla 0..960 x 0..544 ya puesta por gles_present
static int overlay_count;
static void draw_image(cg_image *img, float x, float y, float w, float h, float alpha) {
    if (!img || img->magic != CGIM || !img->rgba || w <= 0 || h <= 0)
        return;
    if (!img->tex) {
        glGenTextures(1, &img->tex);
        glBindTexture(GL_TEXTURE_2D, img->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img->w, img->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img->rgba);
    }
    glBindTexture(GL_TEXTURE_2D, img->tex);
    float xy[8];
    const float px[4] = {x, x + w, x, x + w}, py[4] = {y, y, y + h, y + h};
    for (int i = 0; i < 4; i++)
        gles_view_to_screen(px[i], py[i], &xy[i * 2], &xy[i * 2 + 1]);
    static const float uv[] = {0, 0, 1, 0, 0, 1, 1, 1}; // fila 0 de rgba = arriba
    if (img->premul)
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA), glColor4f(alpha, alpha, alpha, alpha);
    else
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), glColor4f(1, 1, 1, alpha);
    glVertexPointer(2, GL_FLOAT, 0, xy);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    overlay_count++;
}
static cg_image *cg_of(id uiimg) { return uiimg ? ((ui_image *)uiimg)->cg : NULL; }
// Texto del label cacheado en una imagen del tamaño de la vista; se rehace al cambiar texto, color o tamaño.
// ponytail: 1 píxel por punto (coretext no escala glifos con el CTM): se ve algo suave al ampliar a pantalla.
static void draw_label(id v, float x, float y, float w, float h, float alpha) {
    label_ext *e = LX(v);
    if (!e->text || w < 1 || h < 1)
        return;
    if (e->dirty || !e->cache || e->cw != w || e->ch != h) {
        if (e->cache)
            cg_image_release(e->cache);
        int W = (int)ceilf(w), H = (int)ceilf(h);
        cg_ctx *c = cg_BitmapContextCreate(NULL, W, H, 8, 0, (void *)1, 1); // RGBA premultiplicado
        c->ctm = (CGAffineTransform){1, 0, 0, -1, 0, H};                     // UIKit: origen arriba, y hacia abajo
        ui_color_fill(c, e->color);
        ui_PushContext(c);
        union {
            float f;
            uint32_t u;
        } z = {0}, fw = {w}, fh = {h};
        SEL s = sel_registerName("drawTextInRect:");
        objc_call_imp6(objc_lookup_imp(v, s), v, s, z.u, z.u, fw.u, fh.u, 0, 0);
        ui_PopContext();
        e->cache = cg_BitmapCreateImage(c);
        cg_ContextRelease(c);
        e->dirty = 0, e->cw = w, e->ch = h;
        static int logged;
        if (logged++ < 8)
            l_info("UILabel %s: \"%s\" %.0fx%.0f", class_getName(object_getClass(v)), ns_cstr(e->text), w, h);
    }
    draw_image(e->cache, x, y, w, h, alpha);
}
static void draw_view(id v, float ox, float oy, float alpha, int depth) {
    view_ext *x = VX(v);
    if (x->hidden || depth > 16)
        return;
    alpha *= x->alpha;
    if (alpha < 0.01f)
        return;
    float vx = ox + x->frame.o.x, vy = oy + x->frame.o.y, w = x->frame.s.w, h = x->frame.s.h;
    if (objc_is_kind(v, c_UIImageView)) {
        img_ext *e = IX(v);
        id img = e->image;
        int n = e->anim ? ns_array_count(e->anim) : 0;
        if (e->animating && n) {
            double dur = e->anim_dur > 0 ? e->anim_dur : n / 30.0;
            img = ns_array_get(e->anim, (int)(fmod(now_s() - e->anim_t0, dur) / dur * n) % n);
        }
        draw_image(cg_of(img), vx, vy, w, h, alpha);
    } else if (objc_is_kind(v, c_UILabel)) {
        draw_label(v, vx, vy, w, h, alpha);
    } else if (objc_is_kind(v, c_UIButton)) {
        img_ext *e = IX(v);
        draw_image(cg_of(e->bg), vx, vy, w, h, alpha);
        cg_image *im = cg_of(e->image);
        if (im) { // imagen a tamaño natural, centrada (como UIButton)
            float iw = im->w < w ? im->w : w, ih = im->h < h ? im->h : h;
            draw_image(im, vx + (w - iw) / 2, vy + (h - ih) / 2, iw, ih, alpha);
        }
    }
    id subs = x->subviews;
    for (int i = 0, n = subs ? ns_array_count(subs) : 0; i < n; i++)
        draw_view(ns_array_get(subs, i), vx, vy, alpha, depth + 1);
}
void ui_overlay_draw(void) {
    if (!key_window)
        return;
    overlay_count = 0;
    glEnable(GL_BLEND);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    // la ventana y sus hijas directas: origen 0,0 (su frame puede venir apaisado, ver hit_view)
    id subs = VX(key_window)->subviews;
    for (int i = 0, n = ns_array_count(subs); i < n; i++) {
        id s = ns_array_get(subs, i), ss = VX(s)->subviews;
        if (VX(s)->hidden)
            continue;
        for (int j = 0, m = ss ? ns_array_count(ss) : 0; j < m; j++)
            draw_view(ns_array_get(ss, j), 0, 0, VX(s)->alpha, 1);
    }
    glDisable(GL_BLEND);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1, 1, 1, 1);
    static int logged;
    if (overlay_count && !logged++)
        l_info("capa UIKit: %d imágenes dibujadas sobre el frame GL", overlay_count);
}

// El juego paró su animación GL (menú UIKit): re-presentar ~30 fps para que se vean la capa y sus animaciones.
static void overlay_tick(void) {
    static unsigned seen;
    static double last;
    double t = now_s();
    if (frames != seen || !frames) {
        seen = frames, last = t;
    } else if (t - last > 1 / 30.0) {
        last = t;
        gles_present();
    }
}

void uikit_init(void) {
    objc_define("UIResponder", "NSObject", 4, NULL, NULL);
    c_UIView = objc_define("UIView", "UIResponder", 4, uiview_i, uiview_c);
    c_UIWindow = objc_define("UIWindow", "UIView", 4, uiwindow_i, NULL);
    c_CALayer = objc_define("CALayer", "NSObject", 4, calayer_i, NULL);
    c_CAEAGLLayer = objc_define("CAEAGLLayer", "CALayer", 4, NULL, NULL);
    c_EAGLContext = objc_define("EAGLContext", "NSObject", 4, eagl_i, eagl_c);
    objc_define("EAGLSharegroup", "NSObject", 4, NULL, NULL);
    c_UIScreen = objc_define("UIScreen", "NSObject", 4, uiscreen_i, uiscreen_c);
    c_UIApplication = objc_define("UIApplication", "UIResponder", 4, uiapp_i, uiapp_c);
    c_UIDevice = objc_define("UIDevice", "NSObject", 4, uidevice_i, uidevice_c);
    c_UIAccelerometer = objc_define("UIAccelerometer", "NSObject", 4, uiaccm_i, uiaccm_c);
    c_UIAcceleration = objc_define("UIAcceleration", "NSObject", sizeof(ui_accel), uiaccel_i, NULL);
    c_UITouch = objc_define("UITouch", "NSObject", sizeof(ui_touch), uitouch_i, NULL);
    c_UIEvent = objc_define("UIEvent", "NSObject", sizeof(ui_event), uievent_i, NULL);
    c_UIImage = objc_define("UIImage", "NSObject", sizeof(ui_image), uiimage_i, uiimage_c);
    c_UIColor = objc_define("UIColor", "NSObject", sizeof(ui_color), uicolor_i, uicolor_c);
    c_UIFont = objc_define("UIFont", "NSObject", 8, NULL, uifont_c); // coretext.c: {isa, float size} y métodos reales
    c_UIAlertView = objc_define("UIAlertView", "UIView", sizeof(alert), uialert_i, NULL);
    c_UIViewController = objc_define("UIViewController", "UIResponder", 4, uivc_i, NULL);
    c_UITextField = objc_define("UITextField", "UIView", sizeof(text_field), uitf_i, NULL);
    c_MPMoviePlayerController = objc_define("MPMoviePlayerController", "NSObject", 4, mpmovie_i, NULL);
    c_MPMusicPlayerController = objc_define("MPMusicPlayerController", "NSObject", 4, mpmusic_i, mpmusic_c);
    c_UINibDecoder = objc_define("UINibDecoder", "NSObject", sizeof(nib_coder), NULL, NULL);
    c_GLKView = objc_define("GLKView", "UIView", 4, glkview_i, NULL);
    c_GLKViewController = objc_define("GLKViewController", "UIViewController", 4, glkvc_i, NULL);
    c_CADisplayLink = objc_define("CADisplayLink", "NSObject", sizeof(display_link), cadl_i, cadl_c);
    c_UIImageView = objc_define("UIImageView", "UIView", 4, uiimageview_i, NULL);
    objc_define("UIControl", "UIView", 4, NULL, NULL);
    c_UIButton = objc_define("UIButton", "UIControl", 4, uibutton_i, uibutton_c);
    c_UILabel = objc_define("UILabel", "UIView", 4, uilabel_i, NULL);
    objc_define("UIActivityIndicatorView", "UIView", 4, NULL, NULL);
    objc_define("UINavigationController", "UIViewController", 4, NULL, NULL);
    objc_define("UITabBarController", "UIViewController", 4, NULL, NULL);
    objc_define("UIImagePickerController", "UINavigationController", 4, NULL, NULL);
    objc_define("MPMediaPickerController", "UIViewController", 4, NULL, NULL);

    the_app = objc_alloc(c_UIApplication);
    main_screen = objc_alloc(c_UIScreen);
    cur_device = objc_alloc(c_UIDevice);
    accel_obj = objc_alloc(c_UIAccelerometer);
    touch_event = objc_alloc(c_UIEvent);
    k_kEAGLColorFormatRGB565 = ns_string_new("EAGLColorFormat565");
    k_kEAGLColorFormatRGBA8 = ns_string_new("EAGLColorFormatRGBA8");
    k_kEAGLDrawablePropertyColorFormat = ns_string_new("EAGLDrawablePropertyColorFormat");
    k_kEAGLDrawablePropertyRetainedBacking = ns_string_new("EAGLDrawablePropertyRetainedBacking");
#define K(v, s) k_##v = ns_string_new(s)
    K(MPMoviePlayerPlaybackDidFinishNotification, "MPMoviePlayerPlaybackDidFinishNotification");
    K(MPMoviePlayerContentPreloadDidFinishNotification, "MPMoviePlayerContentPreloadDidFinishNotification");
    K(UITextFieldTextDidChangeNotification, "UITextFieldTextDidChangeNotification");
    K(UIApplicationLaunchOptionsRemoteNotificationKey, "UIApplicationLaunchOptionsRemoteNotificationKey");
    K(MPMediaItemPropertyTitle, "title");
    K(MPMediaItemPropertyArtist, "artist");
    K(MPMediaItemPropertyAlbumTitle, "albumTitle");
    K(MPMediaItemPropertyPersistentID, "persistentID");
    K(MPMediaPlaylistPropertyName, "name");
    K(UIKeyboardWillShowNotification, "UIKeyboardWillShowNotification");
    K(UIKeyboardWillHideNotification, "UIKeyboardWillHideNotification");
    K(UIKeyboardFrameEndUserInfoKey, "UIKeyboardFrameEndUserInfoKey");
    K(UIDeviceOrientationDidChangeNotification, "UIDeviceOrientationDidChangeNotification");
    K(UIApplicationDidEnterBackgroundNotification, "UIApplicationDidEnterBackgroundNotification");
    K(UIApplicationWillEnterForegroundNotification, "UIApplicationWillEnterForegroundNotification");
    K(MPMusicPlayerControllerPlaybackStateDidChangeNotification, "MPMusicPlayerControllerPlaybackStateDidChangeNotification");
    K(MPMusicPlayerControllerNowPlayingItemDidChangeNotification, "MPMusicPlayerControllerNowPlayingItemDidChangeNotification");
    K(MPMediaLibraryDidChangeNotification, "MPMediaLibraryDidChangeNotification");
#undef K
}
