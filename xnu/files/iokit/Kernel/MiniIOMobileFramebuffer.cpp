/*
 * MiniIOMobileFramebuffer: a stand-in for Apple's display driver family, as seen by
 * IOMobileFramebuffer.framework (and through it by CoreAnimation's window server).
 *
 * The display is the framebuffer the boot loader left us (the one the kernel's text console
 * draws on). There is no hardware behind it that we drive: a "swap" copies the pixels of the
 * surfaces of its layers into that memory, and "vsync" is a timer.
 *
 * The protocol (selectors and the layout of the swap request) was worked out from the
 * user-space side of iOS 16.5. Everything not implemented is logged once.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/proc.h>

extern "C" {
#include <console/video_console.h>
extern struct vc_info vinfo;            /* the boot framebuffer, as the text console knows it */
}

#include <IOKit/IOBufferMemoryDescriptor.h>

#include "MiniIOSurface.h"

extern "C" void redwood_tl(const char * format, ...) __printflike(1, 2);

/* IOConnectCallMethod selectors of the framebuffer user client (iOS 16.5) */
enum {
	kFBMethodGetLayerDefaultSurface = 3,
	kFBMethodSwapBegin              = 4,
	kFBMethodSwapEnd                = 5,
	kFBMethodSwapWait               = 6,
	kFBMethodGetID                  = 7,
	kFBMethodGetDisplaySize         = 8,
	kFBMethodRequestPowerChange     = 12,
	kFBMethodSwapSignal             = 20,
	kFBMethodGetDigitalOutState     = 25,
	kFBMethodGetDotPitch            = 28,
	kFBMethodGetDisplayArea         = 29,
	kFBMethodSwapCancel             = 52,
	kFBMethodGetColorRemapMode      = 57,
	kFBMethodNotifications          = 72,
	kFBMethodChangeFrameInfo        = 73,
	kFBMethodSupportedFrameInfo     = 74,
	kFBMethodGetCurrentAbsoluteTime = 77,
	kFBMethodGetBlock               = 79,
	kFBMethodSwapCancelAll          = 81,
	kFBMethodGetProtectionOptions   = 82,
};

/* The swap request (IOMobileFramebufferSwapEnd): 0x46c bytes, of which we read these. */
#define SWAP_SIZE               0x46c
#define SWAP_ID                 0x50
#define SWAP_SURFACE(layer)     (0x54 + 4 * (layer))    /* surface number, 0 = none */
#define SWAP_SOURCE(layer)      (0x64 + 16 * (layer))   /* x, y, width, height in the surface */
#define SWAP_ENABLED(layer)     (0xa4 + 4 * (layer))
#define SWAP_DEST(layer)        (0xc4 + 16 * (layer))   /* x, y, width, height on the display */
#define SWAP_LAYERS_SET         0x104                   /* bit per layer given in this swap */
#define SWAP_LAYERS_CLEARED     0x108
#define SWAP_LAYERS             4

#define kFBNotifyVSync          5
#define kFBNotifyTypes          8
#define kFBRefreshHz            60                      /* unless boot-arg fb_hz says otherwise */

#define FRAME_TIMES             64                      /* milliseconds told apart between two frames */
#define FRAME_PAUSE             90                      /* from so many on, nothing was moving: not a slow frame */
#define SWAP_GPU_MS             500                     /* a frame the GPU has not drawn by then is shown as it is */
#define SWAP_HELD_MS            700                     /* a swap that cannot be taken by then is not shown at all */

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint64_t gFBLogged[2];           /* selectors already reported */
static uint8_t  gFBCalls[128];          /* how many calls of each selector were traced */

/*
 * The display user space is told about. The system carries the identity of the iPhone X
 * (see the loader), and iOS knows what screen that phone has: with any other size
 * MobileGestalt reports an unknown screen class and the user interface has no layout for it.
 * So the display is always the iPhone's, and a swap scales the picture to the panel that is
 * really there (in QEMU the loader makes the panel exactly this size and nothing is scaled).
 * Boot-args fbwidth= and fbheight= change the size reported.
 */
#define kFBDefaultWidth         1125
#define kFBDefaultHeight        2436
#define kFBDotsPerInch          458

static TUNABLE(uint32_t, gFBWidth, "fbwidth", kFBDefaultWidth);
static TUNABLE(uint32_t, gFBHeight, "fbheight", kFBDefaultHeight);
/*
 * Boot-arg fbscale=2: the same 375 x 812 points drawn at two pixels a point instead of three.
 * Everything that is drawn is drawn by the processor, and this is 2.25 times fewer pixels to
 * draw; the picture is then enlarged to the panel and is that much less sharp.
 */
#define kFBPointsWide           375
#define kFBPointsHigh           812
#define kFBDefaultScale         3
static TUNABLE(uint32_t, gFBScale, "fbscale", 0);
/* Boot-arg fbtrace=N: log the first N calls of every selector (default 3). */
static TUNABLE(uint32_t, gFBTrace, "fbtrace", 3);

/*
 * A recording of the screen, for a machine one cannot look at while it runs (the phone is
 * reached only through the boot loader, before and after the kernel).
 * Boot-args rec_secs=N [rec_start=S] [rec_mb=M]: from S seconds after the first frame, for N
 * seconds, every frame that is shown is also kept -- one pixel of every REC_STEP in each
 * direction, three bytes a pixel -- in a piece of memory of M megabytes (default 480, which
 * is 520 frames) that is left in place when the kernel returns to the loader. The loader
 * hands it to the host ("xl recording"), where tools/recording.py makes a video of it.
 *
 *   struct rec_header, then per frame: struct rec_frame, width * height pixels (R, G, B)
 *
 * Boot-arg rec_ring=1: when the memory is full the recording does not stop but goes on over its
 * oldest frames, so that what it holds at the end is the last of the run: for something that
 * happens once in a long session, the run is ended right after it (five fingers) and the frames
 * before it are there. The header ("RWREC003") says where the oldest frame is (frames are all
 * the same size, one after the other) and when the recording began, in the kernel's time, for the
 * frames to be matched with what the window server's per-frame lines (glframes=1,
 * userland/src/eagl.c) say. rec_step=N (2 to 8, default 3) keeps one pixel of every N instead of 3:
 * N = 4 holds 1.8 times as many frames, 6 four times. The memory is taken from the system's for the
 * whole run: 640 MB of it under a long session of applications had iOS killing processes for
 * memory, SpringBoard and the window server among them (two respring in one run); 256 did not.
 */
static TUNABLE(uint32_t, gRecSeconds, "rec_secs", 0);
static TUNABLE(uint32_t, gRecStart, "rec_start", 0);
static TUNABLE(uint32_t, gRecMegabytes, "rec_mb", 480);
static TUNABLE(uint32_t, gRecRing, "rec_ring", 0);
static TUNABLE(uint32_t, gRecStep, "rec_step", 3);
static TUNABLE(uint32_t, gDiff, "fb_diff", 0);
/*
 * Boot-arg fb_filter=0: the picture is scaled to the panel nearest neighbour, as it once was. The
 * iPhone's 1125 x 2436 on this panel's 1080 x 2400 is 45 columns and 36 rows fewer: nearest
 * neighbour leaves out every 25th column and every 67th row, and where one of them goes through a
 * letter or a slanting line, that is a step in it or a stroke a pixel thinner -- text and lines a
 * little crooked, here and there and never in the same places twice. By default every pixel of the
 * panel is worked out from the four of the picture around where it falls (bilinear).
 */
static TUNABLE(uint32_t, gFilter, "fb_filter", 1);
static TUNABLE(uint32_t, gRefreshHz, "fb_hz", kFBRefreshHz);
static TUNABLE(uint32_t, gOnTheBeat, "fb_beat", 1);
/*
 * Boot-arg fb_threads=N (1 to 8): so many processors scale a frame onto the panel together (see
 * drawRows()). Three by default: a frame is in the panel's memory 3 ms after its turn instead of 9
 * to 10 (the phone, 15 October; fb_threads=1 is how it was).
 */
static TUNABLE(uint32_t, gThreads, "fb_threads", 3);
#define SWAP_QUEUE              2                       /* frames that may wait for the panel at most (fb_queue) */
static TUNABLE(uint32_t, gQueue, "fb_queue", 2);
static inline unsigned
swapQueue(void)
{
	return gQueue >= 1 && gQueue <= SWAP_QUEUE ? gQueue : 1;
}
/*
 * Boot-arg touch_lat=1: a line for every swap handed over ("tl: s ID at US") and for every frame
 * put on the panel ("tl: v ID ready US shown US done US": the GPU done with it, its beat come, the
 * pixels in the panel's memory), in microseconds of mach_absolute_time, for measuring how long a
 * touch takes to show (tools/latency.py; the touch's own lines: bsd/dev/redwood_input.c).
 */
static TUNABLE(uint32_t, gTouchLat, "touch_lat", 0);

static inline uint64_t
micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / 1000;
}

static inline uint32_t
refreshHz(void)
{
	return gRefreshHz >= 1 && gRefreshHz <= 240 ? gRefreshHz : kFBRefreshHz;
}
static inline uint32_t
recStep(void)
{
	return gRecStep >= 2 && gRecStep <= 8 ? gRecStep : 3;
}
#define REC_STEP        recStep()

struct rec_header {
	char     magic[8];              /* "RWREC003" */
	uint32_t width, height;
	uint32_t frames;
	uint32_t oldest;                /* the place of the oldest frame (rec_ring; 0 until the memory has once been full) */
	uint64_t began;                 /* when the first frame was recorded, in nanoseconds of the kernel's time */
};
struct rec_frame {
	uint64_t nanoseconds;           /* since the first frame of the recording */
};

extern "C" uint64_t loader_recording[3];        /* { physical address, bytes, kernel address }: osfmk/arm64/loader_return.c */

static uint32_t
swapWord(const uint8_t * swap, unsigned offset)
{
	uint32_t value;

	memcpy(&value, swap + offset, sizeof(value));
	return value;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct RowJob;

class IOMobileFramebuffer : public IOService
{
	OSDeclareDefaultStructors(IOMobileFramebuffer);

public:
	IOLock *        fLock;
	uint8_t *       fBase;                  /* the boot framebuffer: the panel */
	uint32_t        fPanelWidth, fPanelHeight, fRowBytes;
	uint32_t        fWidth, fHeight;        /* the display as user space sees it */
	MiniIOSurface * fDefaultSurface;
	uint32_t        fSwapID;
	uint32_t        fSwapsShown;
	uint32_t        fLayersShown;           /* which layers the last swap had a surface in */
	bool            fGraphics;              /* the text console has been told to stop */
	/*
	 * A frame goes to the panel when the GPU is done with it: a thread of ours waits for that
	 * and copies it there, and whoever made the frame is back at the next one meanwhile. One
	 * frame waits at a time: the swap after it is held, in SwapEnd, until this one is being
	 * copied to the panel. Whoever draws is so at most two frames ahead of the panel, and
	 * needs a third surface to be that: CoreAnimation makes itself a new one for every frame.
	 * By default two frames may wait (boot-arg fb_queue=1: one, as before): CoreAnimation draws on a
	 * clock of its own, and a SwapEnd held past its next tick makes that tick late, the clock going
	 * on from there (rc.jobs.fps: 28% of the frames over 16.7 ms with two, 33% with one).
	 */
	IOLock *        fSwapLock;              /* around the next five */
	uint8_t         fSwap[SWAP_QUEUE][SWAP_SIZE];   /* the swaps on their way to the panel (fb_queue), from fSwapFirst on */
	MiniIOSurface * fSwapSurfaces[SWAP_QUEUE][SWAP_LAYERS]; /* ... their surfaces, held from SwapEnd on: their maker may let go of them */
	unsigned        fSwapFirst, fSwapsWaiting;
	/*
	 * ... and not whenever the GPU happens to be done, but on a beat of the refresh rate
	 * (boot-arg fb_beat=0: at once): frames then follow each other at an even pace whatever
	 * each took to draw, as long as there is one for every beat. The beat is this driver's own.
	 * (CoreAnimation does not go by the refresh notifications of this driver -- it never asks
	 * for them -- but by a clock of its own at the rate it reads here; and where the panel's
	 * scan is nobody can read: the display block's register clock is off.) It starts with a
	 * frame that comes later than a beat and a half after the one before -- the first after a
	 * pause, or every one of a renderer that does not keep up, the software one: such a frame
	 * is shown at once, a beat would only make it later still -- and gives way by an eighth of
	 * what a frame is early or late, so that frames are ready some six milliseconds before
	 * their beat: neither a slow one nor a quick one then lands on the wrong side of it.
	 */
	uint64_t        fBeat;                  /* the beat the last frame was shown on */
	uint64_t        fReady;                 /* when the last frame was ready to be shown */
	uint32_t        fStatOnBeat;            /* frames that waited for a refresh, and for how long together */
	uint64_t        fStatBeatWait;
	uint32_t        fStatSurfaces[8];       /* the first different surfaces the bottom layer was given */
	uint32_t        fStatHeld, fStatLost;   /* swaps that waited for the one before; that gave up waiting */
	uint64_t        fSwapTime;              /* when the last swap was handed over */
	uint32_t        fStatSwapsLate;         /* swaps that came more than a refresh and a quarter after the one before (and no pause) */
	uint64_t        fStatWaiting;           /* how long frames waited for the GPU here */
	/* how the display is doing: frames and time spent drawing them here since fStatSince */
	uint32_t        fStatFrames;
	uint64_t        fStatSince, fStatDrawing;
	/* ... and how evenly: the time from each frame to the next, counted by the millisecond (the last count: longer) */
	uint64_t        fLastFrame;
	uint32_t        fFrameTimes[FRAME_TIMES + 1], fFramePauses, fFrameLongest;
	/* boot-arg fb_diff=1: which pixels of the panel each frame changed (it costs: the panel's memory is slow to read) */
	bool            fDiff;
	uint64_t        fDiffPixels;
	uint32_t        fDiffLeft, fDiffTop, fDiffRight, fDiffBottom;
	/* the screen recording, if one was asked for */
	uint32_t *      fColumn;                /* drawLayer(): source column for every panel column */
	uint16_t *      fColumnWeight;          /* ... with fb_filter: how much of the column after it, of 256 */
	uint32_t *      fBlendRow;              /* ... and a row of the picture blended from two (fBlendRowSize pixels) */
	uint32_t        fBlendRowSize;
	uint8_t *       fRecBase;
	uint64_t        fRecSize, fRecUsed;
	uint64_t        fFirstFrame, fRecFirst; /* when the first frame was shown, and the first recorded */
	bool            fRecDone;
	bool            fRecWrapped;            /* (rec_ring) the memory has been full once: frames go over the oldest */

	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	void publishModes(void);
	MiniIOSurface * defaultSurface(void);
	void present(const uint8_t * swap);
	void show(const uint8_t * swap, MiniIOSurface ** surfaces);
	int panelBackBuffer(const uint8_t * swap, MiniIOSurface ** surfaces);
	void presenter(void);
	static void presenterMain(void * self, wait_result_t result);
	void reportFrameTimes(void);
	void drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst);
	void drawRows(const struct RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow);
	static void helperMain(void * self, wait_result_t result);
	/* the scaling shared out (fb_threads): fThreads drawers, the presenter and fThreads - 1 helpers */
	unsigned        fThreads;
	IOLock *        fHelperLock;
	const struct RowJob * fHelperJob;
	uint64_t        fHelperGeneration;
	unsigned        fHelpersBusy, fHelpersStarted;
	uint32_t *      fHelperBlend[7];
	void record(MiniIOSurface * s);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * The display controller behind the framebuffer, on the Galaxy S20+ (y2q, SM8250): its MDSS
 * (Linux: drivers/gpu/drm/msm/disp/dpu1, whose catalog counts from 0xae01000). The firmware set
 * it going -- one source pipe fetching 0x9c000000, through a mixer and interface 1 to the DSI
 * video panel -- and nobody has touched it since. Copying a frame into that one buffer while the
 * panel reads it tears the picture into bands (the copy takes longer than a refresh), so a frame
 * is drawn into the buffer the panel is not showing and the pipe is pointed at it: the address
 * register is double buffered, and what is written to it with a flush of the pipe's bit in the
 * pipe's control path (CTL0) is taken by the hardware at the start of the next frame, the vertical
 * blank. The flush bit clears when that has happened; until then the buffer that was on show is
 * still being read and is not drawn into.
 *
 * The second buffer is the other half of the firmware's "Display Reserved" region (0x9c000000,
 * 36 MB: two frames of 1440 x 3200 fit), which the loader keeps out of the kernel's memory.
 *
 * Boot-arg fb_probe=1: before anything, what the controller is doing, into the loader's log (read
 * only, but for the bus clocks of the registers, turned on if off); fb_probe=2: that, and the pipe
 * pointed at the second buffer -- a copy of the first with its top third green -- for five
 * seconds and back. Boot-arg fb_flip=0: one buffer, as before; fb_flip=2: two (see panelInit() for the third).
 */
extern "C" bool redwood_platform_is(const char * name);
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
#define PANEL_WIMG_WCOMB        0x6             /* VM_WIMG_WCOMB: as the boot framebuffer is mapped (video_console.c) */

static TUNABLE(uint32_t, gProbe, "fb_probe", 0);
static TUNABLE(uint32_t, gFlip, "fb_flip", 3);
/*
 * Boot-arg fb_te=1: the ping-pong block's tear check set as Linux does. Not needed on the y2q: its
 * firmware (DisplayDxe, panel AMB622NP01) sends the panel a frame on every second TE pulse by
 * itself (DSIEnableAutoRefresh, AutoRefreshFrameNumDiv 2: 30 a second of the panel's 60), and what
 * was written here was not seen to change anything (the read pointer stayed 0).
 */
static TUNABLE(uint32_t, gTearCheck, "fb_te", 0);
static TUNABLE(uint32_t, gKick, "fb_kick", 0);
/*
 * Boot-arg fb_hwscale=0: the picture scaled to the panel by the processors, as before. By default
 * the pipes' own scalers do it (see panelScaled()): a frame is only copied, at the display's size.
 */
static TUNABLE(uint32_t, gHwScaleArg, "fb_hwscale", 1);
/*
 * Boot-arg fb_autorefresh=N: the panel sent a frame on every N-th of its TE pulses (default 1: all
 * 60 a second); 0: as the firmware has it (2: 30 a second, half the frames iOS draws never shown).
 */
static TUNABLE(uint32_t, gAutorefresh, "fb_autorefresh", 1);  /* 1: frames sent by us at once, not only by whoever sends them 30 times a second */  /* 0: one buffer; 2: two; 3: three */

#define PANEL_FRONT_PHYS        0x9c000000ull   /* the firmware's framebuffer (y2q: "Display Reserved") */
#define PANEL_BACK_PHYS         0x9d200000ull   /* ... and the second one, in the same region */
#define PANEL_RESERVED_END      0x9e400000ull

#define GCC_DISP_AHB_CBCR       0x0010b00cull   /* gcc_disp_ahb_clk: the bus clock of the display's clock controller */
#define DISPCC_BASE             0x0af00000ull
#define DISPCC_PCLK0_CBCR       0x2004
#define DISPCC_MDP_CBCR         0x200c
#define DISPCC_VSYNC_CBCR       0x2024
#define DISPCC_AHB_CBCR         0x2080          /* the bus clock of the MDSS's registers */
#define DISPCC_MDSS_GDSCR       0x3000
#define CBCR_ENABLE             (1u << 0)
#define CBCR_OFF                (1u << 31)
#define GDSC_POWER_ON           (1u << 31)

#define MDSS_BASE               0x0ae00000ull
#define MDSS_SIZE               0x00085000ull
#define MDP_TOP                 0x01000
#define MDSS_CTL(n)             (0x02000 + 0x200 * (n))
#define CTL_TOP                 0x014
#define CTL_FLUSH               0x018
#define CTL_INTF_ACTIVE         0x0f4
#define CTL_FETCH_PIPE_ACTIVE   0x0fc
#define MDSS_LM(n)              (0x45000 + 0x1000 * (n))
#define MDSS_INTF1              0x6b800
#define MDSS_PINGPONG(n)        (0x71000 + 0x800 * (n))
#define INTF_TEAR_MDP_VSYNC_SEL 0x280           /* (on this controller the tear check is the interface's: Linux's has_intf_te) */
#define INTF_TEAR_CHECK_EN      0x284
#define INTF_TEAR_SYNC_CONFIG_VSYNC 0x288
#define INTF_TEAR_INT_COUNT_VAL 0x298           /* the panel's read pointer */
#define INTF_TEAR_START_POS     0x2a0
#define INTF_TEAR_LINE_COUNT    0x2b0           /* the transfer's write pointer */
#define INTF_TEAR_AUTOREFRESH   0x2b4           /* bit 31: on; bits 0-15: a frame every so many TE pulses */
#define AUTOREFRESH_ON          (1u << 31)
#define PP_TEAR_CHECK_EN        0x000
#define PP_SYNC_CONFIG_VSYNC    0x004           /* bit 19: count lines, bit 20: and start again at the panel's TE */
#define PP_SYNC_CONFIG_HEIGHT   0x008
#define PP_SYNC_WRCOUNT         0x00c
#define PP_VSYNC_INIT_VAL       0x010
#define PP_SYNC_THRESH          0x018
#define PP_START_POS            0x01c
#define PP_RD_PTR_IRQ           0x020
#define PP_INT_COUNT_VAL        0x014           /* the panel's read pointer: frames << 16 | line */
#define PP_LINE_COUNT           0x02c           /* the transfer's write pointer */
#define CTL_START               0x01c
#define MDP_VSYNC_SEL           0x414           /* (in MDP top) */
#define VSYNC_CLOCK_HZ          19200000        /* disp_cc_mdss_vsync_clk: from the crystal */
#define TLMM_NORTH              0x0f900000ull
#define PANEL_TE_GPIO           66              /* dt: qcom,platform-te-gpio (function 1: mdp_vsync), north tile */
#define INTF_HSYNC_CTL          0x008
#define INTF_VSYNC_PERIOD       0x00c
#define INTF_DISPLAY_V_START    0x01c
#define INTF_DISPLAY_V_END      0x024
#define INTF_FRAME_COUNT        0x0ac
#define INTF_LINE_COUNT         0x0b0
#define SSPP_SRC_SIZE           0x00
#define SSPP_OUT_SIZE           0x0c
#define SSPP_OUT_XY             0x10
#define SSPP_SRC0_ADDR          0x14
#define SSPP_SRC_YSTRIDE0       0x24
#define SSPP_SRC_FORMAT         0x30

#define PANEL_PIPES             8
static const uint32_t kPipeBase[PANEL_PIPES] = { 0x05000, 0x07000, 0x09000, 0x0b000, 0x25000, 0x27000, 0x29000, 0x2b000 };
static const uint32_t kPipeFlush[PANEL_PIPES] = { 1u << 0, 1u << 1, 1u << 2, 1u << 18, 1u << 11, 1u << 12, 1u << 24, 1u << 25 };
static const char * const kPipeName[PANEL_PIPES] = { "vig0", "vig1", "vig2", "vig3", "dma0", "dma1", "dma2", "dma3" };

#define PANEL_LATCH_MS          100             /* a flip not taken by then: the panel has stopped, flipping too */
#define PANEL_BUFFERS           3

static uint8_t *        gMdss;                  /* the controller's registers, once its clocks are seen running */
static uint32_t         gPipes;                 /* the pipes that fetch the framebuffer (the firmware: two, a half of the panel each) */
static uint32_t         gPipeFlush;             /* ... their bits in the flush of the CTL path that has them */
static int              gCtl = -1;
static uint64_t         gPanelPhys[PANEL_BUFFERS];      /* the buffers: [0] the firmware's, [1] the second, [2] a third */
static uint8_t *        gPanelVirt[PANEL_BUFFERS];
static unsigned         gPanelBuffers;
static int              gShown;                 /* the buffer the hardware shows (or may: see panelFlip()) */
static int              gPending = -1;          /* ... the one it has been told to show from the next frame on */
static int              gNewest;                /* the last one a frame was drawn into */
static uint64_t         gPendingSince;          /* when the pipe was first told something it has not taken yet */
static uint32_t         gStatFlips, gStatReplaced;      /* flips told the pipe; of them, put in place of one not taken */
static uint32_t         gStatPanelFrame;        /* the interface's frame count at the last report */
static bool             gTe;                    /* the transfers wait for the panel's TE; frames are sent by us (panelKick()) */
static bool             gHwScale;               /* frames are at the display's size and the pipes scale them to the panel */
static bool             gScaled;                /* ... and the pipes have been told so */
static uint32_t         gStatKicks;
static bool             gFlipping;
static volatile uint32_t * gGccDispAhb;         /* the clocks, mapped by panelClocks() for panelAlive() */
static uint8_t *        gDispcc;
static uint32_t         gAlive[4];              /* what panelAlive() last read */

static inline uint32_t
mdssRead(uint32_t offset)
{
	return *(volatile uint32_t *) (gMdss + offset);
}

static inline void
mdssWrite(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *) (gMdss + offset) = value;
}

/*
 * Are the controller's registers safe to read: its power switch on and the bus clocks of its
 * registers running (each turned on if it is not, as Linux keeps them; nothing else is written)?
 * A block without power or clock does not answer, and the processor waits for it for ever.
 */
static bool
panelClocks(void)
{
	vm_offset_t gcc = ml_io_map(trunc_page(GCC_DISP_AHB_CBCR), PAGE_SIZE) + (GCC_DISP_AHB_CBCR & PAGE_MASK);
	uint8_t * dispcc;
	uint32_t ahb, gdsc, bus, mdp, vsync, pclk;

	ahb = *(volatile uint32_t *) gcc;
	if (!(ahb & CBCR_ENABLE)) {
		*(volatile uint32_t *) gcc = ahb | CBCR_ENABLE;
		IODelay(1000);
	}
	if (!(*(volatile uint32_t *) gcc & CBCR_ENABLE)) {
		redwood_tl("fb: panel: the display's clock controller has no bus clock (gcc 0x10b00c %#x): one buffer\n", ahb);
		return false;
	}
	dispcc = (uint8_t *) ml_io_map(DISPCC_BASE, 0x4000);
	gdsc = *(volatile uint32_t *) (dispcc + DISPCC_MDSS_GDSCR);
	bus = *(volatile uint32_t *) (dispcc + DISPCC_AHB_CBCR);
	mdp = *(volatile uint32_t *) (dispcc + DISPCC_MDP_CBCR);
	vsync = *(volatile uint32_t *) (dispcc + DISPCC_VSYNC_CBCR);
	pclk = *(volatile uint32_t *) (dispcc + DISPCC_PCLK0_CBCR);
	if (gProbe) {
		redwood_tl("fb: panel: gcc disp_ahb %#x, dispcc mdss_gdsc %#x ahb %#x mdp %#x vsync %#x pclk0 %#x\n", ahb, gdsc, bus,
		    mdp, vsync, pclk);
	}
	if (!(gdsc & GDSC_POWER_ON) || (mdp & CBCR_OFF)) {
		redwood_tl("fb: panel: the display block is without power or its clock (gdsc %#x mdp %#x): one buffer\n", gdsc, mdp);
		return false;
	}
	if (bus & CBCR_OFF) {
		*(volatile uint32_t *) (dispcc + DISPCC_AHB_CBCR) = bus | CBCR_ENABLE;
		IODelay(1000);
		bus = *(volatile uint32_t *) (dispcc + DISPCC_AHB_CBCR);
		if (bus & CBCR_OFF) {
			redwood_tl("fb: panel: the bus clock of the display block's registers does not run (%#x): one buffer\n", bus);
			return false;
		}
	}
	gGccDispAhb = (volatile uint32_t *) gcc;
	gDispcc = dispcc;
	gAlive[0] = *gGccDispAhb;
	gAlive[1] = gdsc;
	gAlive[2] = bus;
	gAlive[3] = mdp;
	return true;
}

/*
 * Are the controller's power and clocks still as panelClocks() left them? Someone else may take
 * them away while the system runs (a read of the controller then stops the processor for good), so
 * this is asked before the controller is touched: the clock controller's bus clock first (GCC
 * answers always), then what is behind it. A change is said once.
 */
static bool
panelAlive(const char * where)
{
	uint32_t now[4];

	now[0] = *gGccDispAhb;
	if (!(now[0] & CBCR_ENABLE) || (now[0] & CBCR_OFF)) {
		if (now[0] != gAlive[0]) {
			redwood_tl("fb: panel: %s: the display's clock controller has lost its bus clock (gcc 0x10b00c %#x, was %#x)\n", where,
			    now[0], gAlive[0]);
			gAlive[0] = now[0];
		}
		return false;
	}
	now[1] = *(volatile uint32_t *) (gDispcc + DISPCC_MDSS_GDSCR);
	now[2] = *(volatile uint32_t *) (gDispcc + DISPCC_AHB_CBCR);
	now[3] = *(volatile uint32_t *) (gDispcc + DISPCC_MDP_CBCR);
	if (memcmp(now, gAlive, sizeof(now)) != 0) {
		redwood_tl("fb: panel: %s: gcc disp_ahb %#x, mdss_gdsc %#x ahb %#x mdp %#x (were %#x %#x %#x %#x)\n", where, now[0], now[1],
		    now[2], now[3], gAlive[0], gAlive[1], gAlive[2], gAlive[3]);
		memcpy(gAlive, now, sizeof(now));
	}
	return (now[1] & GDSC_POWER_ON) && !(now[2] & CBCR_OFF) && !(now[3] & CBCR_OFF);
}

/* fb_probe: what the firmware left the controller doing (false: its clocks went away meanwhile) */
static bool
panelSurvey(void)
{
	uint32_t frames[2], lines[2], line, frame;
	uint64_t times[2];

	redwood_tl("fb: panel: mdss hw rev %#x, mdp top hw rev %#x\n", mdssRead(0), mdssRead(MDP_TOP));
	line = mdssRead(MDSS_INTF1 + INTF_HSYNC_CTL) >> 16;
	frame = mdssRead(MDSS_INTF1 + INTF_VSYNC_PERIOD);
	redwood_tl("fb: panel: intf1 en %#x config %#x hsync %#x vsync_period %u v_start %u v_end %u hctl %#x config2 %#x "
	    "count_en %#x\n", mdssRead(MDSS_INTF1), mdssRead(MDSS_INTF1 + 4), mdssRead(MDSS_INTF1 + INTF_HSYNC_CTL), frame,
	    mdssRead(MDSS_INTF1 + INTF_DISPLAY_V_START), mdssRead(MDSS_INTF1 + INTF_DISPLAY_V_END), mdssRead(MDSS_INTF1 + 0x3c),
	    mdssRead(MDSS_INTF1 + 0x60), mdssRead(MDSS_INTF1 + 0xa8));
	for (int i = 0; i < 2; i++) {
		frames[i] = mdssRead(MDSS_INTF1 + INTF_FRAME_COUNT);
		lines[i] = mdssRead(MDSS_INTF1 + INTF_LINE_COUNT);
		absolutetime_to_nanoseconds(mach_absolute_time(), &times[i]);
		for (int tenth = 0; i == 0 && tenth < 10; tenth++) {
			redwood_tl("fb: panel: survey: tenth %d (frame %u)\n", tenth, frames[0]);
			IOSleep(100);
			redwood_tl("fb: panel: survey: tenth %d slept\n", tenth);
			if (!panelAlive("survey")) {
				return false;
			}
		}
	}
	redwood_tl("fb: panel: intf1 frame %u line %u, then frame %u line %u %llu us later (%u lines a frame)\n", frames[0], lines[0],
	    frames[1], lines[1], (times[1] - times[0]) / 1000, line ? frame / line : 0);
	if (times[1] > times[0]) {
		/* (a command-mode panel: no timing, the frames counted as they are sent) */
		uint64_t sent = (uint64_t) (frames[1] - frames[0]) * 1000000000ull * 1000 / (times[1] - times[0]);

		redwood_tl("fb: panel: intf1 counted %llu.%03llu frames a second\n", sent / 1000, sent % 1000);
	}
	if (line != 0 && frame >= line && times[1] > times[0]) {
		uint64_t per = frame / line, gone = (uint64_t) (frames[1] - frames[0]) * per + lines[1] - lines[0];
		uint64_t rate = gone * 1000000000ull / per * 1000 / (times[1] - times[0]);

		redwood_tl("fb: panel: THE PANEL IS SENT %llu.%03llu FRAMES A SECOND\n", rate / 1000, rate % 1000);
	}
	for (int n = 0; n < 6; n++) {
		uint32_t c = MDSS_CTL(n);

		redwood_tl("fb: panel: ctl%d layer %#x %#x %#x %#x %#x %#x top %#x flush %#x ext %#x %#x intf_active %#x pipe_active %#x "
		    "%#x %#x\n", n, mdssRead(c), mdssRead(c + 4), mdssRead(c + 8), mdssRead(c + 0xc), mdssRead(c + 0x10),
		    mdssRead(c + 0x24), mdssRead(c + CTL_TOP), mdssRead(c + CTL_FLUSH), mdssRead(c + 0x40), mdssRead(c + 0x44),
		    mdssRead(c + CTL_INTF_ACTIVE), mdssRead(c + CTL_FETCH_PIPE_ACTIVE), mdssRead(c + 0x12c), mdssRead(c + 0x130));
	}
	for (int n = 0; n < 2; n++) {
		uint32_t b = MDSS_PINGPONG(n);

		redwood_tl("fb: panel: pingpong%d tear_en %#x vsync %#x height %#x wrcount %#x init %#x int_count %#x thresh %#x start %#x "
		    "rd_ptr %#x out_line %u line %u autorefresh %#x dsc %#x\n", n, mdssRead(b), mdssRead(b + 4), mdssRead(b + 8),
		    mdssRead(b + 0xc), mdssRead(b + 0x10), mdssRead(b + 0x14), mdssRead(b + 0x18), mdssRead(b + 0x1c), mdssRead(b + 0x20),
		    mdssRead(b + 0x28), mdssRead(b + 0x2c), mdssRead(b + 0x30), mdssRead(b + 0xa0));
	}
	for (int n = 0; n < 6; n++) {
		redwood_tl("fb: panel: lm%d op_mode %#x out_size %#x\n", n, mdssRead(MDSS_LM(n)), mdssRead(MDSS_LM(n) + 4));
	}
	for (int p = 0; p < PANEL_PIPES; p++) {
		uint32_t b = kPipeBase[p];

		redwood_tl("fb: panel: %s src_size %#x src_xy %#x out_size %#x out_xy %#x addr %#x stride %u format %#x unpack %#x "
		    "op_mode %#x\n", kPipeName[p], mdssRead(b), mdssRead(b + 8), mdssRead(b + 0xc), mdssRead(b + 0x10),
		    mdssRead(b + SSPP_SRC0_ADDR), mdssRead(b + SSPP_SRC_YSTRIDE0), mdssRead(b + SSPP_SRC_FORMAT), mdssRead(b + 0x34),
		    mdssRead(b + 0x38));
	}
	return true;
}

/*
 * Send the panel a frame now (it is a command-mode panel: it keeps the last one it was sent and
 * shows it from its own memory). With the tear check on, the controller holds the transfer until
 * the panel's scan is out of its way. What the pipe was flushed with is taken at its start.
 */
static void
panelKick(void)
{
	mdssWrite(MDSS_CTL(gCtl) + CTL_START, 1);
	gStatKicks++;
}

/* Has the pipe taken what it was last told (the flush bits clear)? Then that buffer is the one shown. */
static void
panelUpdate(void)
{
	if (gPending >= 0 && !(mdssRead(MDSS_CTL(gCtl) + CTL_FLUSH) & gPipeFlush)) {
		gShown = gPending;
		gPending = -1;
	}
}

/*
 * The pipes' scalers (QSEED3, version 3.0 on this controller: Linux's dpu_hw_setup_scaler3() with
 * what _dpu_plane_setup_scaler3() works out for an RGB picture). The firmware has each pipe fetch
 * its part of the framebuffer at the panel's size -- two pipes, a half of the panel each, the
 * second one's output at x 720 (the mixers split the source) -- and nothing is scaled. Scaled, a
 * pipe fetches the columns of the display's picture that its half of the panel shows and enlarges
 * them, bilinear: the step from one panel pixel to the next is display over panel, in 21 bits of
 * fraction, and where a half begins between two source columns the scaler starts with that
 * fraction (the second half: at 562.5 of 1125). The registers are double buffered like the
 * address: what is written here is taken with the next flush, the address with it.
 */
#define SSPP_SCALER             0xa00
#define QSEED3_OP_ENABLE        (1u << 0)
#define QSEED3_OP_Y_RGB_BIL     (3u << 16)      /* DPU_SCALE_BIL */
#define QSEED3_OP_ALPHA_EN      (1u << 10)
#define QSEED3_OP_ALPHA_BIL     (1u << 29)      /* DPU_SCALE_ALPHA_BIL */
#define QSEED3_OP_NOT_DX        (1u << 14)
#define QSEED3_OP_BLEND         (1u << 31)
#define QSEED3_PRELOAD_VALUE    (4u | 2u << 8 | 4u << 16 | 2u << 24)    /* DPU_QSEED4_DEFAULT_PRELOAD_H, _V */
#define PHASE_SHIFT             21
#define SRC_FORMAT_ALPHA        (1u << 8)       /* SRCC3_EN: the format has alpha */

/* every register of a pipe that scaling changes: the pipe's rectangles, its pixel extension, its scaler */
static const uint16_t kPipeScaleRegs[] = {
	0x000, 0x008, 0x00c, 0x010,                                             /* src size, src xy, out size, out xy */
	0x100, 0x104, 0x108, 0x110, 0x114, 0x118, 0x120, 0x124, 0x128,         /* SW_PIX_EXT C0, C1C2, C3: LR, TB, REQ_PIXELS */
	SSPP_SCALER + 0x04, SSPP_SCALER + 0x10, SSPP_SCALER + 0x14, SSPP_SCALER + 0x18, SSPP_SCALER + 0x1c, SSPP_SCALER + 0x20,
	SSPP_SCALER + 0x40, SSPP_SCALER + 0x44, SSPP_SCALER + 0x48,
	SSPP_SCALER + 0x90, SSPP_SCALER + 0x94, SSPP_SCALER + 0x98, SSPP_SCALER + 0x9c,
};
#define PIPE_SCALE_REGS         (sizeof(kPipeScaleRegs) / sizeof(kPipeScaleRegs[0]))
static uint32_t gPipeFirmware[PANEL_PIPES][PIPE_SCALE_REGS];   /* as the firmware left them */
static uint32_t gPipeScaledRegs[PANEL_PIPES][PIPE_SCALE_REGS]; /* ... and scaled */

static void
pipeScaledSet(uint32_t * regs, uint32_t offset, uint32_t value)
{
	for (unsigned i = 0; i < PIPE_SCALE_REGS; i++) {
		if (kPipeScaleRegs[i] == offset) {
			regs[i] = value;
		}
	}
}

/*
 * Work out the scaled registers of the pipes, for a picture of `width` x `height` on the panel's
 * `panelWidth` x `panelHeight`. False: these pipes cannot do it (a DMA pipe has no scaler, or a
 * pipe's part of the panel is not a whole column of it).
 */
static bool
panelScaled(uint32_t width, uint32_t height, uint32_t panelWidth, uint32_t panelHeight)
{
	uint32_t stepX = (uint32_t) (((uint64_t) width << PHASE_SHIFT) / panelWidth);
	uint32_t stepY = (uint32_t) (((uint64_t) height << PHASE_SHIFT) / panelHeight);

	if (width > panelWidth || height > panelHeight || (gPipes & ~0xfu) != 0) {
		return false;
	}
	for (int p = 0; p < PANEL_PIPES; p++) {
		uint32_t b = kPipeBase[p], * regs = gPipeScaledRegs[p], outX, outW, outH, srcX, srcW, last, init, op;
		uint64_t first;

		if (!(gPipes & (1u << p))) {
			continue;
		}
		for (unsigned i = 0; i < PIPE_SCALE_REGS; i++) {
			gPipeFirmware[p][i] = regs[i] = mdssRead(b + kPipeScaleRegs[i]);
		}
		outX = mdssRead(b + SSPP_OUT_XY) & 0xffff;
		outW = mdssRead(b + SSPP_OUT_SIZE) & 0xffff;
		outH = mdssRead(b + SSPP_OUT_SIZE) >> 16;
		if ((mdssRead(b + SSPP_OUT_XY) >> 16) != 0 || outH != panelHeight || outW == 0 || outX + outW > panelWidth) {
			redwood_tl("fb: panel: %s shows %ux%u at %#x of the panel: not scaled by the pipes\n", kPipeName[p], outW, outH,
			    mdssRead(b + SSPP_OUT_XY));
			return false;
		}
		/* the source columns its panel columns fall on: from the one under the first, to the one after the last's */
		first = (uint64_t) outX * stepX;
		srcX = (uint32_t) (first >> PHASE_SHIFT);
		init = (uint32_t) (first & ((1u << PHASE_SHIFT) - 1));
		last = (uint32_t) (((uint64_t) (outX + outW - 1) * stepX) >> PHASE_SHIFT) + 2;
		srcW = (last < width ? last : width) - srcX;
		op = QSEED3_OP_ENABLE | QSEED3_OP_Y_RGB_BIL | QSEED3_OP_NOT_DX | QSEED3_OP_BLEND;
		if (mdssRead(b + SSPP_SRC_FORMAT) & SRC_FORMAT_ALPHA) {
			op |= QSEED3_OP_ALPHA_EN | QSEED3_OP_ALPHA_BIL;
		}
		pipeScaledSet(regs, SSPP_SRC_SIZE, height << 16 | srcW);
		pipeScaledSet(regs, 0x008, srcX);
		for (uint32_t pe = 0x100; pe <= 0x128; pe += 4) {
			pipeScaledSet(regs, pe, (pe & 0xf) == 8 ? (height << 16 | srcW) : 0);
		}
		pipeScaledSet(regs, SSPP_SCALER + 0x04, op);
		pipeScaledSet(regs, SSPP_SCALER + 0x10, stepX);
		pipeScaledSet(regs, SSPP_SCALER + 0x14, stepY);
		pipeScaledSet(regs, SSPP_SCALER + 0x18, stepX);
		pipeScaledSet(regs, SSPP_SCALER + 0x1c, stepY);
		pipeScaledSet(regs, SSPP_SCALER + 0x20, QSEED3_PRELOAD_VALUE);
		pipeScaledSet(regs, SSPP_SCALER + 0x40, height << 16 | srcW);
		pipeScaledSet(regs, SSPP_SCALER + 0x44, height << 16 | srcW);
		pipeScaledSet(regs, SSPP_SCALER + 0x48, outH << 16 | outW);
		pipeScaledSet(regs, SSPP_SCALER + 0x90, init);
		pipeScaledSet(regs, SSPP_SCALER + 0x94, 0);
		pipeScaledSet(regs, SSPP_SCALER + 0x98, init);
		pipeScaledSet(regs, SSPP_SCALER + 0x9c, 0);
		redwood_tl("fb: panel: %s: scaler version %#x (op mode %#x); scaled: columns %u to %u of %u (phase %#x) onto %u at %u, "
		    "step %#x x %#x\n", kPipeName[p], mdssRead(b + SSPP_SCALER), gPipeFirmware[p][13], srcX, srcX + srcW - 1, width, init,
		    outW, outX, stepX, stepY);
	}
	return true;
}

/* The pipes told to scale (or to show the panel's size as the firmware had them), from the next flush on. */
static void
panelScale(bool scaled)
{
	for (int p = 0; p < PANEL_PIPES; p++) {
		if (gPipes & (1u << p)) {
			for (unsigned i = 0; i < PIPE_SCALE_REGS; i++) {
				mdssWrite(kPipeBase[p] + kPipeScaleRegs[i], scaled ? gPipeScaledRegs[p][i] : gPipeFirmware[p][i]);
			}
		}
	}
	gScaled = scaled;
}

/*
 * Point the pipe at buffer `which`: taken by the hardware at the next frame's start. A flip told
 * before and not taken yet is replaced: its buffer is never shown and is free again -- unless a
 * frame began while the pipe was being told (the interface's frame count moved), when it may have
 * been taken just before; it is then counted as shown, until the pipe is seen to take this one.
 */
static void
panelFlip(unsigned which)
{
	uint32_t before, after, waiting;

	__builtin_arm_dsb(0xf);                 /* (the frame's pixels in memory before the hardware is told of them) */
	before = mdssRead(MDSS_INTF1 + INTF_FRAME_COUNT);
	waiting = mdssRead(MDSS_CTL(gCtl) + CTL_FLUSH) & gPipeFlush;
	if (gHwScale != gScaled) {
		panelScale(gHwScale);           /* (the first frame at the display's size; or, on the way out, the panel's again) */
	}
	for (int p = 0; p < PANEL_PIPES; p++) {
		if (gPipes & (1u << p)) {
			mdssWrite(kPipeBase[p] + SSPP_SRC0_ADDR, (uint32_t) gPanelPhys[which]);
		}
	}
	mdssWrite(MDSS_CTL(gCtl) + CTL_FLUSH, gPipeFlush);
	if (gTe && gKick) {
		panelKick();
	}
	after = mdssRead(MDSS_INTF1 + INTF_FRAME_COUNT);
	gStatFlips++;
	gStatReplaced += gPending >= 0 && waiting && after == before;
	if (gPending < 0 || !waiting || after != before) {
		if (gPending >= 0) {
			gShown = gPending;
		}
		gPendingSince = mach_absolute_time();
	}
	gPending = (int) which;
	gNewest = (int) which;
}

/* Has the last flip been taken (the buffer shown before it no longer read)? Waits up to `ms`. */
static bool
panelLatched(uint32_t ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
	while (mdssRead(MDSS_CTL(gCtl) + CTL_FLUSH) & gPipeFlush) {
		if (mach_absolute_time() > deadline) {
			return false;
		}
		IODelay(100);
	}
	panelUpdate();
	return true;
}

/* fb_probe=2: the second buffer on show for five seconds, and back */
static void
panelFlipTest(uint32_t rowBytes, uint32_t width, uint32_t height)
{
	uint64_t began, latched;
	uint32_t lineAt, frameAt;
	bool taken;

	for (uint32_t y = 0; y < height; y++) {
		uint32_t * to = (uint32_t *) (gPanelVirt[1] + (size_t) y * rowBytes);

		if (y < height / 3) {
			for (uint32_t x = 0; x < width; x++) {
				to[x] = 0xff00c000;
			}
		} else {
			memcpy(to, gPanelVirt[0] + (size_t) y * rowBytes, (size_t) width * 4);
		}
	}
	began = mach_absolute_time();
	panelFlip(1);
	taken = panelLatched(PANEL_LATCH_MS);
	latched = mach_absolute_time();
	lineAt = mdssRead(MDSS_INTF1 + INTF_LINE_COUNT);
	frameAt = mdssRead(MDSS_INTF1 + INTF_FRAME_COUNT);
	redwood_tl("fb: panel: flip test: the pipe at %#llx, %s after %llu us (intf1 frame %u line %u); the top third is green "
	    "for five seconds\n", gPanelPhys[1], taken ? "taken" : "NOT TAKEN", micros(latched) - micros(began), frameAt, lineAt);
	for (int tenth = 0; tenth < 50; tenth++) {
		IOSleep(100);
		if (!panelAlive("flip test")) {
			return;
		}
	}
	redwood_tl("fb: panel: flip test: intf1 frame %u; back to the firmware's buffer\n", mdssRead(MDSS_INTF1 + INTF_FRAME_COUNT));
	panelFlip(0);
	taken = panelLatched(PANEL_LATCH_MS);
	redwood_tl("fb: panel: flip test: back %s\n", taken ? "(taken)" : "NOT TAKEN");
}

/*
 * How often the panel is sent a frame. Its firmware has the interface do it by itself, on every
 * second TE pulse of the panel's 60 (AutoRefreshFrameNumDiv 2 in DisplayDxe's AMB622NP01): 30
 * frames a second, and of the 60 that iOS draws a second, half are never shown -- uneven motion,
 * and up to 33 ms before a frame is. A frame is sent in some 8 ms (DSC at 3:1 over four lanes at
 * 1157 Mbit/s), well inside a refresh, so the interface is told to send one on every pulse.
 */
static void
panelAutorefresh(void)
{
	uint32_t config = mdssRead(MDSS_INTF1 + INTF_TEAR_AUTOREFRESH), frames[2];
	uint64_t times[2];

	redwood_tl("fb: panel: intf1 tear check: en %#x vsync_sel %#x sync %#x read pointer %#x start %u write pointer %u; "
	    "autorefresh %#x\n", mdssRead(MDSS_INTF1 + INTF_TEAR_CHECK_EN), mdssRead(MDSS_INTF1 + INTF_TEAR_MDP_VSYNC_SEL),
	    mdssRead(MDSS_INTF1 + INTF_TEAR_SYNC_CONFIG_VSYNC), mdssRead(MDSS_INTF1 + INTF_TEAR_INT_COUNT_VAL),
	    mdssRead(MDSS_INTF1 + INTF_TEAR_START_POS), mdssRead(MDSS_INTF1 + INTF_TEAR_LINE_COUNT), config);
	if (!(config & AUTOREFRESH_ON) || gAutorefresh == 0 || gAutorefresh > 0xffff || (config & 0xffff) == gAutorefresh) {
		return;
	}
	mdssWrite(MDSS_INTF1 + INTF_TEAR_AUTOREFRESH, AUTOREFRESH_ON | gAutorefresh);
	for (int i = 0; i < 2; i++) {
		if (i == 1) {
			IODelay(500000);
		}
		frames[i] = mdssRead(MDSS_INTF1 + INTF_FRAME_COUNT);
		times[i] = mach_absolute_time();
	}
	redwood_tl("fb: panel: autorefresh %#x now (was %#x): %u frames sent in %llu ms\n", mdssRead(MDSS_INTF1 + INTF_TEAR_AUTOREFRESH),
	    config, frames[1] - frames[0], (micros(times[1]) - micros(times[0])) / 1000);
}

/*
 * The panel's TE line: a pulse at the start of each of its own refreshes. Polled for 100 ms, its
 * rising edges are counted: how many times a second the panel really refreshes, and whether it
 * says so at all (0: no TE, or the pin is not the controller's).
 */
static uint32_t
panelTeRate(void)
{
	uint64_t phys = TLMM_NORTH + 0x1000ull * PANEL_TE_GPIO;
	volatile uint32_t * pin = (volatile uint32_t *) (ml_io_map(trunc_page(phys), PAGE_SIZE) + (phys & PAGE_MASK));
	uint32_t config = pin[0], edges = 0, high = 0, polls = 0, level, was;
	uint64_t began = mach_absolute_time(), now, until;

	if ((config >> 2 & 0xf) != 1) {
		redwood_tl("fb: panel: GPIO%u is not the panel's TE (config %#x): frames are sent as before\n", PANEL_TE_GPIO, config);
		return 0;
	}
	nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &until);
	until += began;
	was = pin[1] & 1;
	do {
		level = pin[1] & 1;
		edges += level && !was;
		high += level;
		polls++;
		was = level;
		now = mach_absolute_time();
	} while (now < until);
	redwood_tl("fb: panel: GPIO%u (TE, config %#x): %u rising edges in 100 ms, high in %u of %u reads; MDP vsync_sel %#x\n",
	    PANEL_TE_GPIO, config, edges, high, polls, mdssRead(MDP_TOP + MDP_VSYNC_SEL));
	return edges * 10;
}

/*
 * The tear check of the ping-pong block behind the mixers (Linux's dpu_hw_pp_enable_te() with
 * what dpu_encoder_phys_cmd_tearcheck_config() works out): the block counts the panel's lines
 * from its TE on and starts a transfer only where the panel's scan does not run into it. The
 * line rate it counts with is the vsync clock over lines a frame and frames a second.
 */
static void
panelTearCheck(uint32_t teHz, uint32_t height)
{
	uint32_t pp = MDSS_PINGPONG(0), total = height + 32, count = VSYNC_CLOCK_HZ / (total * teHz);

	mdssWrite(pp + PP_SYNC_CONFIG_VSYNC, (1u << 19) | (1u << 20) | count);
	mdssWrite(pp + PP_SYNC_CONFIG_HEIGHT, total * 2);
	mdssWrite(pp + PP_VSYNC_INIT_VAL, height);
	mdssWrite(pp + PP_RD_PTR_IRQ, height + 1);
	mdssWrite(pp + PP_START_POS, height);
	mdssWrite(pp + PP_SYNC_THRESH, 4u << 16 | 4);
	mdssWrite(pp + PP_SYNC_WRCOUNT, height + 4 + 1);
	mdssWrite(pp + PP_TEAR_CHECK_EN, 1);
	redwood_tl("fb: panel: tear check on (pingpong0: %u lines of %u vsync clocks, %u a second)\n", total, count, teHz);
	IODelay(20000);
	redwood_tl("fb: panel: pingpong0's read pointer now %#x, write pointer %u\n", mdssRead(pp + PP_INT_COUNT_VAL),
	    mdssRead(pp + PP_LINE_COUNT));
}

/*
 * Find the pipe that shows the framebuffer and get the second buffer ready. Two buffers only on
 * the y2q, its controller running, a pipe fetching the framebuffer whose rows are the framebuffer's.
 */
static void
panelInit(uint32_t rowBytes, uint32_t width, uint32_t height, uint32_t displayWidth, uint32_t displayHeight)
{
	uint64_t size = (uint64_t) rowBytes * height;

	if (!redwood_platform_is("y2q") || vinfo.v_physaddr != PANEL_FRONT_PHYS || PANEL_BACK_PHYS + size > PANEL_RESERVED_END) {
		return;
	}
	if (!panelClocks()) {
		return;
	}
	gMdss = (uint8_t *) ml_io_map(MDSS_BASE, MDSS_SIZE);
	if (gProbe && !panelSurvey()) {
		redwood_tl("fb: panel: one buffer\n");
		return;
	}
	for (int p = 0; p < PANEL_PIPES; p++) {
		if (mdssRead(kPipeBase[p] + SSPP_SRC0_ADDR) == (uint32_t) PANEL_FRONT_PHYS) {
			if ((mdssRead(kPipeBase[p] + SSPP_SRC_YSTRIDE0) & 0xffff) != rowBytes) {
				gPipes = 0;
				break;
			}
			gPipes |= 1u << p;
			gPipeFlush |= kPipeFlush[p];
		}
	}
	/* the CTL path whose mixers take that pipe: the first active one (the firmware has one) */
	for (int n = 0; n < 6 && gCtl < 0; n++) {
		if (mdssRead(MDSS_CTL(n) + CTL_INTF_ACTIVE) != 0 || mdssRead(MDSS_CTL(n) + CTL_TOP) != 0) {
			gCtl = n;
		}
	}
	if (gPipes == 0 || gCtl < 0) {
		redwood_tl("fb: panel: no pipe shows the framebuffer as it is (pipes %#x ctl %d): one buffer\n", gPipes, gCtl);
		return;
	}
	gPanelPhys[0] = PANEL_FRONT_PHYS;
	gPanelPhys[1] = PANEL_BACK_PHYS;
	gPanelVirt[0] = (uint8_t *) vinfo.v_baseaddr;
	/* (18 MB: more than the kernel's I/O map holds, which ml_io_map() takes from) */
	gPanelVirt[1] = (uint8_t *) ml_io_map_unmappable(PANEL_BACK_PHYS, size, PANEL_WIMG_WCOMB);
	gPanelBuffers = 2;
	if (gFlip >= 3) {
		/*
		 * A third, from the kernel's memory: the panel takes a frame some 30 times a second, and
		 * with two buffers the next frame cannot be drawn until the last flip is taken -- up to
		 * 33 ms of waiting for every frame, the swaps behind it held up. With three there is always
		 * one neither shown nor about to be. (The pipe's address register has 32 bits.)
		 */
		IOBufferMemoryDescriptor * third = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapWriteCombineCache, size, 0x00000000fffff000ull);

		if (third != NULL && third->getPhysicalSegment(0, NULL) + size <= 0x100000000ull) {
			gPanelPhys[2] = third->getPhysicalSegment(0, NULL);
			gPanelVirt[2] = (uint8_t *) third->getBytesNoCopy();
			gPanelBuffers = 3;
		} else {
			redwood_tl("fb: panel: no memory under 4 GB for a third buffer: two\n");
		}
	}
	gShown = gNewest = 0;
	gPending = -1;
	redwood_tl("fb: panel: the framebuffer is fetched by pipes %#x (ctl%d, flush bits %#x); %s (%u buffers%s%llx)\n", gPipes, gCtl,
	    gPipeFlush, gFlip ? "frames are drawn into another buffer and flipped to at a frame's start" : "one buffer (fb_flip=0)",
	    gPanelBuffers, gPanelBuffers > 2 ? ", the third at " : "", gPanelBuffers > 2 ? gPanelPhys[2] : 0);
	if (gProbe >= 2) {
		panelFlipTest(rowBytes, width, height);
	}
	gFlipping = gFlip != 0;
	gHwScale = gFlipping && gHwScaleArg && (displayWidth != width || displayHeight != height) &&
	    panelScaled(displayWidth, displayHeight, width, height);
	panelAutorefresh();
	if (gFlipping && gTearCheck) {
		uint32_t teHz = panelTeRate();

		if (teHz >= 20 && teHz <= 130) {
			panelTearCheck(teHz, height);
			gTe = true;
		}
	}
}

/* On the way back to the loader: the firmware's buffer on show again, which is the one the loader and a panic draw on. */
extern "C" void
MiniIOMobileFramebufferQuiesce(void)
{
	if (gMdss != NULL && gPipes != 0 && (gShown != 0 || gNewest != 0 || gScaled) && panelAlive("the return to the loader")) {
		gHwScale = false;               /* (and the pipes at the panel's size again: the loader draws at it) */
		panelFlip(0);
	}
}

static void
setNumber(OSDictionary * dict, const char * key, uint64_t value)
{
	OSNumber * n = OSNumber::withNumber(value, 32);

	if (n != NULL) {
		dict->setObject(key, n);
		n->release();
	}
}

/*
 * The modes of the display, the way CoreAnimation reads them (the properties TimingElements
 * and ColorElements; without a mode it draws nothing). One timing, the panel's size at
 * 60 Hz (boot-arg fb_hz=N: at N), with one colour mode, 8-bit RGB. The identifiers are what "the current mode"
 * (block 0x40) answers with.
 */
#define kFBTimingID     1
#define kFBColorID      1

void
IOMobileFramebuffer::publishModes(void)
{
	OSDictionary * timing = OSDictionary::withCapacity(8);
	OSDictionary * horizontal = OSDictionary::withCapacity(4);
	OSDictionary * vertical = OSDictionary::withCapacity(4);
	OSDictionary * color = OSDictionary::withCapacity(8);
	OSArray * timings = OSArray::withCapacity(1);
	OSArray * colors = OSArray::withCapacity(1);
	const uint32_t rate = refreshHz() << 16;                /* 16.16 fixed point */

	if (timing == NULL || horizontal == NULL || vertical == NULL || color == NULL || timings == NULL || colors == NULL) {
		return;
	}
	setNumber(color, "ID", kFBColorID);
	setNumber(color, "PixelEncoding", 0);                   /* RGB 4:4:4 */
	setNumber(color, "Depth", 8);
	setNumber(color, "Colorimetry", 1);
	setNumber(color, "EOTF", 1);
	setNumber(color, "DynamicRange", 0);
	setNumber(color, "Score", 1);
	color->setObject("IsVirtual", kOSBooleanFalse);
	colors->setObject(color);

	setNumber(horizontal, "Active", fWidth);
	setNumber(horizontal, "Total", fWidth);
	setNumber(horizontal, "SyncRate", rate);
	setNumber(horizontal, "PreciseSyncRate", rate);
	setNumber(vertical, "Active", fHeight);
	setNumber(vertical, "Total", fHeight);
	setNumber(vertical, "SyncRate", rate);
	setNumber(vertical, "PreciseSyncRate", rate);
	setNumber(timing, "ID", kFBTimingID);
	setNumber(timing, "Score", 1);
	timing->setObject("IsVirtual", kOSBooleanFalse);
	timing->setObject("IsOverscanned", kOSBooleanFalse);
	timing->setObject("HorizontalAttributes", horizontal);
	timing->setObject("VerticalAttributes", vertical);
	timing->setObject("ColorModes", colors);
	timings->setObject(timing);

	setProperty("TimingElements", timings);
	setProperty("ColorElements", colors);
	OSSafeReleaseNULL(timings);
	OSSafeReleaseNULL(colors);
	OSSafeReleaseNULL(timing);
	OSSafeReleaseNULL(horizontal);
	OSSafeReleaseNULL(vertical);
	OSSafeReleaseNULL(color);
}

bool
IOMobileFramebuffer::start(IOService * provider)
{
	thread_t thread;

	if (!IOService::start(provider)) {
		return false;
	}
	if (vinfo.v_baseaddr == 0 || vinfo.v_width == 0 || vinfo.v_height == 0 || vinfo.v_depth != 32) {
		IOLog("MiniIOMobileFramebuffer: no usable boot framebuffer (%ux%u, depth %u)\n", vinfo.v_width, vinfo.v_height,
		    vinfo.v_depth);
		return false;
	}
	fLock = IOLockAlloc();
	fSwapLock = IOLockAlloc();
	if (kernel_thread_start(&IOMobileFramebuffer::presenterMain, this, &thread) != KERN_SUCCESS) {
		IOLog("MiniIOMobileFramebuffer: no thread to put frames on the panel with\n");
		return false;
	}
	thread_set_thread_name(thread, "frames to the panel");
	thread_deallocate(thread);
	fThreads = gThreads >= 1 && gThreads <= 8 ? gThreads : 1;
	fHelperLock = IOLockAlloc();
	for (unsigned i = 0; i + 1 < fThreads; i++) {
		if (kernel_thread_start(&IOMobileFramebuffer::helperMain, this, &thread) != KERN_SUCCESS) {
			fThreads = i + 1;
			break;
		}
		thread_set_thread_name(thread, "frames to the panel: a helper");
		thread_deallocate(thread);
	}
	fBase = (uint8_t *) vinfo.v_baseaddr;
	fPanelWidth = vinfo.v_width;
	fPanelHeight = vinfo.v_height;
	fRowBytes = vinfo.v_rowbytes;
	fWidth = gFBWidth ? gFBWidth : fPanelWidth;
	fHeight = gFBHeight ? gFBHeight : fPanelHeight;
	if (gFBScale != 0 && gFBScale != kFBDefaultScale) {
		static const char * const places[][2] = { { "/chosen", "display-scale" }, { "/product", "artwork-scale-factor" } };
		uint32_t scale = gFBScale;

		fWidth = kFBPointsWide * scale;
		fHeight = kFBPointsHigh * scale;
		/* what the loader's device tree says about the scale is where MobileGestalt has it from */
		for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
			IORegistryEntry * node = IORegistryEntry::fromPath(places[i][0], gIODTPlane);

			if (node != NULL) {
				node->setProperty(places[i][1], &scale, sizeof(scale));
				node->release();
			}
		}
	}
	panelInit(fRowBytes, fPanelWidth, fPanelHeight, fWidth, fHeight);
	if (gHwScale) {
		/* (the pipes scale the frames to the panel: here they are drawn at the display's size, into its top left) */
		IOLog("MiniIOMobileFramebuffer: the panel's %ux%u is scaled to by its pipes\n", fPanelWidth, fPanelHeight);
		fPanelWidth = fWidth;
		fPanelHeight = fHeight;
	}
	fColumn = IONewZero(uint32_t, fPanelWidth);
	fColumnWeight = IONewZero(uint16_t, fPanelWidth);
	fDiff = gDiff != 0;
	fDiffLeft = fDiffTop = ~0u;
	IOLog("MiniIOMobileFramebuffer: display %ux%u on a panel of %ux%u (%u bytes per row)\n", fWidth, fHeight,
	    fPanelWidth, fPanelHeight, fRowBytes);

	if (gRecSeconds != 0) {
		/* one piece of physical memory: the loader will send it over USB as it lies */
		IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous, (mach_vm_size_t) gRecMegabytes << 20, ~0ULL);

		if (memory != NULL) {
			struct rec_header * header = (struct rec_header *) memory->getBytesNoCopy();

			fRecBase = (uint8_t *) header;
			fRecSize = (uint64_t) gRecMegabytes << 20;
			fRecUsed = sizeof(*header);
			bzero(header, sizeof(*header));
			memcpy(header->magic, "RWREC003", 8);
			header->width = fWidth / REC_STEP;
			header->height = fHeight / REC_STEP;
			loader_recording[0] = memory->getPhysicalSegment(0, NULL);
			loader_recording[1] = fRecUsed;
			loader_recording[2] = (uint64_t) (uintptr_t) fRecBase;
			IOLog("MiniIOMobileFramebuffer: recording %u s of the screen at %ux%u from second %u, into %u MB at %#llx%s\n",
			    gRecSeconds, header->width, header->height, gRecStart, gRecMegabytes, loader_recording[0],
			    gRecRing ? ", the last of it kept (rec_ring)" : "");
		} else {
			IOLog("MiniIOMobileFramebuffer: no memory for a recording of %u MB\n", gRecMegabytes);
		}
	}

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	/* for who draws with the GPU (userland/src/eagl.c): a frame need not be waited for before it is handed over */
	setProperty("RedwoodWaitsForGPU", kOSBooleanTrue);
	publishModes();
	setName("disp0");
	registerService();
	return true;
}

/* Keep a small copy of the frame that is about to be shown (the surface is the whole screen). */
void
IOMobileFramebuffer::record(MiniIOSurface * s)
{
	struct rec_header * header = (struct rec_header *) fRecBase;
	uint64_t now = mach_absolute_time(), since_first, since_start = 0, frame_size;
	struct rec_frame * frame;
	uint8_t * to;

	if (fFirstFrame == 0) {
		fFirstFrame = now;
	}
	if (fRecBase == NULL || fRecDone || s->info.width < fWidth || s->info.height < fHeight) {
		return;
	}
	absolutetime_to_nanoseconds(now - fFirstFrame, &since_first);
	if (since_first < (uint64_t) gRecStart * NSEC_PER_SEC) {
		return;
	}
	if (fRecFirst == 0) {
		uint64_t began;

		fRecFirst = now;
		absolutetime_to_nanoseconds(now, &began);
		header->began = began;
		IOLog("MiniIOMobileFramebuffer: the recording begins at %llu.%03llu s of the kernel's time (mach_absolute_time)\n",
		    began / NSEC_PER_SEC, began / NSEC_PER_MSEC % 1000);
	}
	absolutetime_to_nanoseconds(now - fRecFirst, &since_start);
	frame_size = sizeof(*frame) + (uint64_t) header->width * header->height * 3;
	if (since_start > (uint64_t) gRecSeconds * NSEC_PER_SEC || (fRecUsed + frame_size > fRecSize && !gRecRing)) {
		fRecDone = true;
		IOLog("MiniIOMobileFramebuffer: recording finished: %u frames, %llu MB\n", header->frames, fRecUsed >> 20);
		return;
	}
	if (fRecUsed + frame_size > fRecSize) {
		/* (rec_ring) round again: over the oldest, which is the next one */
		fRecUsed = sizeof(*header);
		fRecWrapped = true;
	}
	frame = (struct rec_frame *) (fRecBase + fRecUsed);
	frame->nanoseconds = since_start;
	to = (uint8_t *) (frame + 1);
	for (uint32_t y = 0; y < header->height; y++) {
		const uint32_t * from = (const uint32_t *) (s->pixels() + (size_t) (y * REC_STEP + 1) * s->info.bytesPerRow);

		for (uint32_t x = 0; x < header->width; x++) {
			uint32_t pixel = from[x * REC_STEP + 1];        /* B G R A in memory */

			*to++ = (uint8_t) (pixel >> 16);
			*to++ = (uint8_t) (pixel >> 8);
			*to++ = (uint8_t) pixel;
		}
	}
	fRecUsed += frame_size;
	if (!fRecWrapped) {
		header->frames++;
		loader_recording[1] = fRecUsed;
	} else {
		/* the memory full of frames: all of them are handed over, from the one after this */
		header->oldest = (uint32_t) ((fRecUsed - sizeof(*header)) / frame_size);
		if (header->oldest >= header->frames) {
			header->oldest = 0;
		}
		loader_recording[1] = sizeof(*header) + (uint64_t) header->frames * frame_size;
	}
}

MiniIOSurface *
IOMobileFramebuffer::defaultSurface(void)
{
	IOLockLock(fLock);
	if (fDefaultSurface == NULL) {
		fDefaultSurface = MiniIOSurfaceCreateSimple(fWidth, fHeight, FOURCC('B', 'G', 'R', 'A'));
	}
	IOLockUnlock(fLock);
	return fDefaultSurface;
}

/*
 * Two pixels mixed: `w` of 256 of the second. Two of the four channels at a time, in one word.
 */
static inline uint32_t
blend(uint32_t a, uint32_t b, uint32_t w)
{
	uint32_t rb = ((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8 & 0xff00ff;
	uint32_t ga = ((a >> 8 & 0xff00ff) * (256 - w) + (b >> 8 & 0xff00ff) * w) & 0xff00ff00;

	return rb | ga;
}

/*
 * Where the middle of the `i`th of `out` pixels falls among `in` of them, in 256ths of a pixel,
 * kept within the first and the last.
 */
static inline int64_t
sample_at(int64_t i, int64_t in, int64_t out)
{
	int64_t at = ((2 * i + 1) * in - out) * 128 / out;

	return at < 0 ? 0 : at > (in - 1) * 256 ? (in - 1) * 256 : at;
}

/*
 * The rows of one layer, from `y0` to `y1` (panel rows), as drawLayer() has worked them out:
 * scaling a whole frame takes one processor some 9 ms, so with boot-arg fb_threads=N it is shared
 * out between N of them, each with rows of its own and a row of blending of its own.
 */
struct RowJob {
	const uint8_t * pixels;
	uint32_t        stride;
	int32_t         sx, sy, sw, sh, px0, py0, px1, py1, cx0, cx1;
	bool            filter, unscaled;
};

void
IOMobileFramebuffer::drawRows(const RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow)
{
	const uint8_t * pixels = job->pixels;
	uint32_t stride = job->stride;
	int32_t sx = job->sx, sy = job->sy, sw = job->sw, sh = job->sh;
	int32_t px0 = job->px0, py0 = job->py0, px1 = job->px1, py1 = job->py1, cx0 = job->cx0, cx1 = job->cx1;
	bool filter = job->filter;

	for (int32_t y = y0; y < y1; y++) {
		const uint32_t * from;
		uint32_t * to;

		if (y < 0 || (uint32_t) y >= fPanelHeight) {
			continue;
		}
		to = (uint32_t *) (fBase + (size_t) y * fRowBytes);
		if (job->unscaled) {
			/* not scaled at all */
			memcpy(to + px0, pixels + (size_t) (sy + y - py0) * stride + (size_t) sx * 4, (size_t) sw * 4);
			continue;
		}
		if (filter) {
			int64_t at = sample_at(y - py0, sh, py1 - py0);
			uint32_t row = (uint32_t) (at >> 8), weight = (uint32_t) (at & 255);
			const uint32_t * upper = (const uint32_t *) (pixels + (size_t) (sy + row) * stride) + sx;

			if (weight == 0 || row + 1 >= (uint32_t) sh) {
				from = upper;
			} else {
				const uint32_t * lower = (const uint32_t *) (pixels + (size_t) (sy + row + 1) * stride) + sx;
				uint32_t first = fColumn[cx0], last = fColumn[cx1 - 1] + 1 < (uint32_t) sw ? fColumn[cx1 - 1] + 1 : fColumn[cx1 - 1];

				uint32_t * __restrict mixed = blendRow;

				for (uint32_t c = first; c <= last; c++) {
					uint32_t a = upper[c], b = lower[c];

					mixed[c] = a == b ? a : blend(a, b, weight);           /* (most of a screen is the same as next to it) */
				}
				from = blendRow;
			}
		} else {
			from = (const uint32_t *) (pixels + (size_t) (sy + (int64_t) (y - py0) * sh / (py1 - py0)) * stride);
		}
		if (fDiff) {
			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t value = filter ? blend(from[fColumn[x]], from[fColumn[x] + (fColumnWeight[x] != 0)], fColumnWeight[x]) :
				    from[fColumn[x]];

				if (to[x] != value) {
					fDiffPixels++;
					fDiffLeft = (uint32_t) x < fDiffLeft ? (uint32_t) x : fDiffLeft;
					fDiffRight = (uint32_t) x > fDiffRight ? (uint32_t) x : fDiffRight;
					fDiffTop = (uint32_t) y < fDiffTop ? (uint32_t) y : fDiffTop;
					fDiffBottom = (uint32_t) y > fDiffBottom ? (uint32_t) y : fDiffBottom;
				}
			}
		}
		if (filter) {
			const uint32_t * __restrict column = fColumn + cx0;
			const uint16_t * __restrict weights = fColumnWeight + cx0;
			uint32_t * __restrict out = to + cx0;

			for (uint32_t i = 0, n = (uint32_t) (cx1 - cx0); i < n; i++) {
				uint32_t c = column[i], w = weights[i], a = from[c];

				out[i] = w == 0 || a == from[c + 1] ? a : blend(a, from[c + 1], w);
			}
		} else {
			for (int32_t x = cx0; x < cx1; x++) {
				to[x] = from[fColumn[x]];
			}
		}
	}
	__builtin_arm_dsb(0xf);                 /* (these rows in memory before the flip that shows them, made on another processor) */
}

/* A helper of the scaling (fb_threads): waits for rows to do, does them, says so. */
void
IOMobileFramebuffer::helperMain(void * self, wait_result_t result __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;
	unsigned index;
	uint64_t done = 0;

	IOLockLock(fb->fHelperLock);
	index = fb->fHelpersStarted++;
	IOLockUnlock(fb->fHelperLock);
	for (;;) {
		const RowJob * job;
		int32_t rows, y0, y1;
		uint32_t * blend;

		IOLockLock(fb->fHelperLock);
		while (fb->fHelperGeneration == done) {
			IOLockSleep(fb->fHelperLock, &fb->fHelperGeneration, THREAD_UNINT);
		}
		done = fb->fHelperGeneration;
		job = fb->fHelperJob;
		IOLockUnlock(fb->fHelperLock);
		/* share index + 1 of fThreads (the presenter takes share 0) */
		rows = job->py1 - job->py0;
		y0 = job->py0 + (int32_t) ((int64_t) rows * (index + 1) / fb->fThreads);
		y1 = job->py0 + (int32_t) ((int64_t) rows * (index + 2) / fb->fThreads);
		blend = fb->fHelperBlend[index];
		fb->drawRows(job, y0, y1, blend);
		IOLockLock(fb->fHelperLock);
		fb->fHelpersBusy--;
		if (fb->fHelpersBusy == 0) {
			IOLockWakeup(fb->fHelperLock, &fb->fHelpersBusy, true);
		}
		IOLockUnlock(fb->fHelperLock);
	}
}

/*
 * Put a rectangle of a 32-bit surface on the screen. `dst` is in display coordinates; the
 * panel may be of another size, and the layer itself may be scaled: both are done in one
 * step, bilinear (or nearest neighbour: fb_filter=0).
 */
void
IOMobileFramebuffer::drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst)
{
	int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
	int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
	int32_t px0, py0, px1, py1, cx0, cx1;
	bool filter = gFilter != 0;
	RowJob job;

	if (sw <= 0 || sh <= 0) {
		sx = sy = 0;
		sw = (int32_t) s->info.width;
		sh = (int32_t) s->info.height;
	}
	if (dw <= 0 || dh <= 0) {
		dx = dy = 0;
		dw = (int32_t) fWidth;
		dh = (int32_t) fHeight;
	}
	if (sx < 0 || sy < 0 || (uint32_t) (sx + sw) > s->info.width || (uint32_t) (sy + sh) > s->info.height) {
		return;
	}
	/* the rectangle on the panel */
	px0 = (int32_t) ((int64_t) dx * fPanelWidth / fWidth);
	py0 = (int32_t) ((int64_t) dy * fPanelHeight / fHeight);
	px1 = (int32_t) ((int64_t) (dx + dw) * fPanelWidth / fWidth);
	py1 = (int32_t) ((int64_t) (dy + dh) * fPanelHeight / fHeight);
	if (px1 <= px0 || py1 <= py0) {
		return;
	}
	/* (blending, a row of the picture mixed from two: room for it, for every one who draws rows) */
	if (filter && fBlendRowSize < (uint32_t) sw) {
		if (fBlendRow != NULL) {
			IODelete(fBlendRow, uint32_t, fBlendRowSize);
		}
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			if (fHelperBlend[i] != NULL) {
				IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
			}
		}
		fBlendRowSize = (uint32_t) sw;
		fBlendRow = IONew(uint32_t, fBlendRowSize);
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			fHelperBlend[i] = IONew(uint32_t, fBlendRowSize);
			if (fHelperBlend[i] == NULL && fBlendRow != NULL) {
				IODelete(fBlendRow, uint32_t, fBlendRowSize);
				fBlendRow = NULL;
			}
		}
		if (fBlendRow == NULL) {
			fBlendRowSize = 0;
		}
	}
	filter = filter && fBlendRow != NULL;
	/* which pixel of a source row each pixel of a panel row takes: worked out once, not 2.6 million times */
	cx0 = px0 < 0 ? 0 : px0;
	cx1 = (uint32_t) px1 > fPanelWidth ? (int32_t) fPanelWidth : px1;
	for (int32_t x = cx0; x < cx1; x++) {
		if (filter) {
			int64_t at = sample_at(x - px0, sw, px1 - px0);

			fColumn[x] = (uint32_t) (at >> 8);              /* (in the blended row: from sx on) */
			fColumnWeight[x] = (uint16_t) (fColumn[x] + 1 < (uint32_t) sw ? at & 255 : 0);
		} else {
			fColumn[x] = (uint32_t) (sx + (int64_t) (x - px0) * sw / (px1 - px0));
		}
	}
	job.pixels = s->pixels();
	job.stride = s->info.bytesPerRow;
	job.sx = sx;
	job.sy = sy;
	job.sw = sw;
	job.sh = sh;
	job.px0 = px0;
	job.py0 = py0;
	job.px1 = px1;
	job.py1 = py1;
	job.cx0 = cx0;
	job.cx1 = cx1;
	job.filter = filter;
	job.unscaled = px1 - px0 == sw && py1 - py0 == sh && px0 >= 0 && (uint32_t) px1 <= fPanelWidth;
	if (fThreads <= 1 || fDiff || fHelpersStarted + 1 < fThreads) {
		drawRows(&job, py0, py1, fBlendRow);
		return;
	}
	/* shared out: the helpers take their rows, this thread the first share, then waits for them */
	IOLockLock(fHelperLock);
	fHelperJob = &job;
	fHelpersBusy = fThreads - 1;
	fHelperGeneration++;
	IOLockWakeup(fHelperLock, &fHelperGeneration, false);
	IOLockUnlock(fHelperLock);
	drawRows(&job, py0, py0 + (int32_t) ((int64_t) (py1 - py0) / fThreads), fBlendRow);
	IOLockLock(fHelperLock);
	while (fHelpersBusy != 0) {
		IOLockSleep(fHelperLock, &fHelpersBusy, THREAD_UNINT);
	}
	IOLockUnlock(fHelperLock);
}

/*
 * How evenly the frames of the last seconds came: how many followed the one before within the
 * time of one refresh at 120, 60, 40 and 30 a second, and the times half and 95 in 100 of them
 * stayed under. With the lock held.
 */
void
IOMobileFramebuffer::reportFrameTimes(void)
{
	static const uint32_t limits[] = { 10, 18, 26, 35, FRAME_TIMES + 1 };
	uint32_t within[sizeof(limits) / sizeof(limits[0])] = { 0 }, total = 0, sum = 0, half = 0, most = 0;

	for (uint32_t ms = 0; ms <= FRAME_TIMES; ms++) {
		for (uint32_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
			if (ms < limits[i]) {
				within[i] += fFrameTimes[ms];
				break;
			}
		}
		total += fFrameTimes[ms];
	}
	for (uint32_t ms = 0; ms <= FRAME_TIMES && total != 0; ms++) {
		sum += fFrameTimes[ms];
		if (half == 0 && 2 * sum >= total) {
			half = ms + 1;
		}
		if (most == 0 && 100 * sum >= 95 * total) {
			most = ms + 1;
		}
	}
	if (total != 0) {
		IOLog("MiniIOMobileFramebuffer: from one frame to the next: %u times under 10 ms, %u under 18, %u under 26, %u under 35, "
		    "%u longer (the longest %u ms); half under %u ms, 95 in 100 under %u ms; %u pauses\n", within[0], within[1],
		    within[2], within[3], within[4], fFrameLongest, half, most, fFramePauses);
	}
	bzero(fFrameTimes, sizeof(fFrameTimes));
	fFramePauses = fFrameLongest = 0;
}

/* SwapEnd: the swap is handed to the thread that puts frames on the panel, as soon as that has no other waiting. */
void
IOMobileFramebuffer::present(const uint8_t * swap)
{
	AbsoluteTime deadline;

	uint64_t now = mach_absolute_time(), period, pause;

	/* (a frame that is late on the panel was late here already, or it was not: for telling whose it is) */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz() * 5 / 4, &period);
	nanoseconds_to_absolutetime(FRAME_PAUSE * NSEC_PER_MSEC, &pause);
	clock_interval_to_deadline(SWAP_HELD_MS, kMillisecondScale, &deadline);
	if (gTouchLat) {
		redwood_tl("tl: s %u at %llu\n", swapWord(swap, SWAP_ID), micros(now));
	}
	IOLockLock(fSwapLock);
	fStatSwapsLate += fSwapTime != 0 && now - fSwapTime > period && now - fSwapTime < pause;
	fSwapTime = now;
	fStatHeld += fSwapsWaiting >= swapQueue();
	while (fSwapsWaiting >= swapQueue()) {
		if (IOLockSleepDeadline(fSwapLock, &fSwapsWaiting, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    fSwapsWaiting >= swapQueue()) {
			fStatLost++;                    /* (the GPU has stopped answering: the frame before is waiting for it) */
			IOLockUnlock(fSwapLock);
			return;
		}
	}
	unsigned slot = (fSwapFirst + fSwapsWaiting) % SWAP_QUEUE;

	memcpy(fSwap[slot], swap, SWAP_SIZE);
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));

		fSwapSurfaces[slot][layer] = sid != 0 ? MiniIOSurfaceCopy(sid) : NULL;
	}
	fSwapsWaiting++;
	IOLockWakeup(fSwapLock, fSwap, true);
	IOLockUnlock(fSwapLock);
}

void
IOMobileFramebuffer::presenterMain(void * self, wait_result_t result __unused)
{
	((IOMobileFramebuffer *) self)->presenter();
}

void
IOMobileFramebuffer::presenter(void)
{
	static uint8_t swap[SWAP_SIZE];         /* (one thread) */
	MiniIOSurface * surfaces[SWAP_LAYERS];

	for (;;) {
		IOLockLock(fSwapLock);
		while (fSwapsWaiting == 0) {
			IOLockSleep(fSwapLock, fSwap, THREAD_UNINT);
		}
		memcpy(swap, fSwap[fSwapFirst], SWAP_SIZE);
		memcpy(surfaces, fSwapSurfaces[fSwapFirst], sizeof(surfaces));
		IOLockUnlock(fSwapLock);
		show(swap, surfaces);
		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			if (surfaces[layer] != NULL) {
				MiniIOSurfaceDrop(surfaces[layer]);
			}
		}
		MiniIOSurfaceClearKept();               /* (the next frame is some milliseconds away) */
	}
}

/*
 * Where a frame is drawn: with two buffers (panelInit()), into the one not on show, once the panel
 * has stopped reading it -- the flip away from it taken at a frame's start; its number, for
 * panelFlip() when the frame is in it. A frame whose bottom layer does not cover the whole panel
 * leaves some of the buffer as it was, frames ago: the newest one is copied into it first
 * (slowly: that memory is not cached). -1: one buffer, the one on show.
 */
int
IOMobileFramebuffer::panelBackBuffer(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	MiniIOSurface * s = surfaces[0];
	int32_t src[4], dst[4];
	uint64_t waited;
	int back;
	bool covered;

	if (!gFlipping) {
		return -1;
	}
	if (!panelAlive("a frame")) {
		/* (the controller is off: nothing is on show, whichever buffer the frame goes to) */
		fBase = gPanelVirt[gNewest];
		return -1;
	}
	panelUpdate();
	if (gTe && gKick && gPending >= 0) {
		panelKick();                    /* (the last kick came while a frame was being sent, and was not taken) */
	}
	if (gPending >= 0 && gPanelBuffers == 2) {
		panelLatched(PANEL_LATCH_MS);
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - gPendingSince, &waited);
	if (gPending >= 0 && waited >= PANEL_LATCH_MS * NSEC_PER_MSEC) {
		redwood_tl("fb: panel: a flip was not taken in %u ms: one buffer from now on\n", PANEL_LATCH_MS);
		gFlipping = false;
		fBase = gPanelVirt[gPending];
		return -1;
	}
	for (back = 0; back == gShown || back == gPending; back++) {
	}
	fBase = gPanelVirt[back];
	memcpy(src, swap + SWAP_SOURCE(0), sizeof(src));
	memcpy(dst, swap + SWAP_DEST(0), sizeof(dst));
	if (swapWord(swap, SWAP_SURFACE(0)) == 0) {
		covered = (swapWord(swap, SWAP_LAYERS_SET) & 1) && fGraphics;       /* (made black: see show()) */
	} else {
		covered = s != NULL && s->info.bytesPerElement == 4 && s->info.planeCount == 0 &&
		    (src[2] <= 0 || src[3] <= 0 || (src[0] >= 0 && src[1] >= 0 && (uint32_t) (src[0] + src[2]) <= s->info.width &&
		    (uint32_t) (src[1] + src[3]) <= s->info.height)) &&
		    (dst[2] <= 0 || dst[3] <= 0 || (dst[0] <= 0 && dst[1] <= 0 && (uint32_t) (dst[0] + dst[2]) >= fWidth &&
		    (uint32_t) (dst[1] + dst[3]) >= fHeight));
	}
	if (!covered) {
		for (uint32_t y = 0; y < fPanelHeight; y++) {
			memcpy(fBase + (size_t) y * fRowBytes, gPanelVirt[gNewest] + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
		}
	}
	return back;
}

/* A swap's surfaces onto the panel, once the GPU is done with them. */
void
IOMobileFramebuffer::show(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	uint32_t set = swapWord(swap, SWAP_LAYERS_SET);
	uint64_t began = mach_absolute_time(), nanos, ready, shown, drawn;
	uint32_t layers = 0;
	MiniIOSurface * recorded = NULL;
	bool report, drew = false;
	int back;

	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (surfaces[layer] != NULL && !MiniIOSurfaceDrawn(surfaces[layer], SWAP_GPU_MS)) {
			IOLog("MiniIOMobileFramebuffer: swap %u: the GPU is not done with surface %u after %u ms; shown as it is\n",
			    swapWord(swap, SWAP_ID), swapWord(swap, SWAP_SURFACE(layer)), SWAP_GPU_MS);
		}
	}
	fStatWaiting += mach_absolute_time() - began;
	began = ready = mach_absolute_time();
	if (gOnTheBeat && !gFlipping) {
		/* (with buffers flipped the pipe takes a frame at the panel's own beat: none of ours is needed) */
		uint64_t period, aim, next;

		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		aim = period * 3 / 8;
		next = fReady;
		fReady = began;
		if (fBeat == 0 || began - next > period * 3 / 2) {
			fBeat = began;
		} else {
			for (next = fBeat + period; next < began; next += period) {
			}
			if (next - began > aim) {
				next -= (next - began - aim) / 8;
			} else {
				next += (aim - (next - began)) / 8;
			}
			clock_delay_until(next);
			fBeat = next;
			fStatOnBeat++;
			fStatBeatWait += mach_absolute_time() - began;
			began = mach_absolute_time();
		}
	}
	shown = began;
	/* its turn has come: the swap after it may be handed over */
	IOLockLock(fSwapLock);
	fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
	fSwapsWaiting--;
	IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
	IOLockUnlock(fSwapLock);
	IOLockLock(fLock);
	if (fLastFrame != 0) {
		absolutetime_to_nanoseconds(began - fLastFrame, &nanos);
		nanos /= NSEC_PER_MSEC;
		if (nanos >= FRAME_PAUSE) {
			fFramePauses++;
		} else {
			fFrameTimes[nanos < FRAME_TIMES ? nanos : FRAME_TIMES]++;
			fFrameLongest = nanos > fFrameLongest ? (uint32_t) nanos : fFrameLongest;
		}
	}
	fLastFrame = began;
	report = fSwapsShown < 8;
	back = panelBackBuffer(swap, surfaces);
	if (report) {
		IOLog("MiniIOMobileFramebuffer: swap %u (#%u): surfaces %u %u %u %u, layers set 0x%x cleared 0x%x\n",
		    swapWord(swap, SWAP_ID), fSwapsShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
		    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)), set, swapWord(swap, SWAP_LAYERS_CLEARED));
	}
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
		int32_t src[4], dst[4];
		MiniIOSurface * s;

		layers |= sid != 0 ? 1u << layer : 0;
		/* the frames' surfaces: is it a few, in turn? */
		for (unsigned i = 0; layer == 0 && sid != 0 && i < 8; i++) {
			if (fStatSurfaces[i] == 0 || fStatSurfaces[i] == sid) {
				fStatSurfaces[i] = sid;
				break;
			}
		}
		if (sid == 0) {
			/* the bottom layer taken away: nothing is shown, the screen is black */
			if (layer == 0 && (set & 1) && fGraphics) {
				for (uint32_t y = 0; y < fPanelHeight; y++) {
					bzero(fBase + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
				}
				drew = true;
			}
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		s = surfaces[layer];
		if (report) {
			IOLog("MiniIOMobileFramebuffer: swap %u layer %u: surface %u%s (%ux%u '%c%c%c%c') "
			    "from %d,%d %dx%d to %d,%d %dx%d, layers set 0x%x\n", swapWord(swap, SWAP_ID), layer, sid,
			    s ? "" : " MISSING", s ? s->info.width : 0, s ? s->info.height : 0,
			    s ? (int) (s->info.pixelFormat >> 24 & 0xff) : '?', s ? (int) (s->info.pixelFormat >> 16 & 0xff) : '?',
			    s ? (int) (s->info.pixelFormat >> 8 & 0xff) : '?', s ? (int) (s->info.pixelFormat & 0xff) : '?',
			    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], set);
		}
		if (s == NULL) {
			continue;
		}
		if (s->info.bytesPerElement == 4 && s->info.planeCount == 0) {
			if (!fGraphics) {
				/* the first picture: the kernel's text console must stop drawing over it */
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
			}
			drawLayer(s, src, dst);
			drew = true;
			if (layer == 0) {
				recorded = s;   /* (recorded once the frame is in the panel's memory: see below) */
			}
		}
	}
	if (drew && back >= 0) {
		panelFlip((unsigned) back);     /* (on show from the next frame's start) */
	}
	drawn = mach_absolute_time();
	if (recorded != NULL) {
		record(recorded);
	}
	fSwapsShown++;
	if (gTouchLat) {
		redwood_tl("tl: v %u ready %llu shown %llu done %llu\n", swapWord(swap, SWAP_ID), micros(ready), micros(shown),
		    micros(drawn));
	}
	if (layers != fLayersShown) {
		IOLog("MiniIOMobileFramebuffer: swap #%u: surfaces in layers 0x%x now (0x%x before): %u %u %u %u\n", fSwapsShown, layers,
		    fLayersShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)), swapWord(swap, SWAP_SURFACE(2)),
		    swapWord(swap, SWAP_SURFACE(3)));
		fLayersShown = layers;
	}

	/* every five seconds in which something was drawn: how many frames, and what each cost here */
	fStatFrames++;
	fStatDrawing += mach_absolute_time() - began;
	if (fStatSince == 0) {
		fStatSince = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - fStatSince, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t drawing, waiting, beat;
		unsigned different;

		absolutetime_to_nanoseconds(fStatDrawing, &drawing);
		absolutetime_to_nanoseconds(fStatWaiting, &waiting);
		absolutetime_to_nanoseconds(fStatBeatWait, &beat);
		IOLog("MiniIOMobileFramebuffer: %u frames in %llu ms (%llu a second), %llu us each in the driver, after %llu us "
		    "waiting for the GPU and, %u of them, %llu us for the next refresh; %u swaps waited for the frame before, "
		    "%u gave up; %u swaps came a refresh late themselves\n", fStatFrames, nanos / NSEC_PER_MSEC,
		    fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
		    waiting / fStatFrames / NSEC_PER_USEC, fStatOnBeat, fStatOnBeat ? beat / fStatOnBeat / NSEC_PER_USEC : 0,
		    fStatHeld, fStatLost, fStatSwapsLate);
		fStatSwapsLate = 0;
		if (gFlipping) {
			uint32_t panelFrame = mdssRead(MDSS_INTF1 + INTF_FRAME_COUNT);

			/* (the kernel's own log does not reach the loader's in a whole boot) */
			redwood_tl("fb: %u frames in %llu ms, %llu us each in the driver; %u flips, %u of them in place of one the panel had "
			    "not taken; the panel was sent %u frames (%u kicks)\n", fStatFrames, nanos / NSEC_PER_MSEC,
			    drawing / fStatFrames / NSEC_PER_USEC, gStatFlips, gStatReplaced, gStatPanelFrame != 0 ? panelFrame - gStatPanelFrame : 0,
			    gStatKicks);
			gStatPanelFrame = panelFrame;
			gStatFlips = gStatReplaced = gStatKicks = 0;
		}
		for (different = 0; different < 8 && fStatSurfaces[different] != 0; different++) {
		}
		if (different == 8 && fStatFrames > 8) {
			uint32_t made, freed, again;
			uint64_t making, freeing;

			/* (CoreAnimation has made itself a new surface for every frame: see "the frames' surfaces" above) */
			MiniIOSurfaceCost(&made, &making, &freed, &freeing, &again);
			IOLog("MiniIOMobileFramebuffer: the frames came in surfaces %u, %u, %u, %u, %u and on, none twice; in that time "
			    "%u surfaces of all kinds were made, in %llu us each (%u of them of memory kept from one before), and %u "
			    "freed, in %llu us each\n", fStatSurfaces[0], fStatSurfaces[1], fStatSurfaces[2], fStatSurfaces[3],
			    fStatSurfaces[4], made, making, again, freed, freeing);
		}
		bzero(fStatSurfaces, sizeof(fStatSurfaces));
		fStatHeld = fStatLost = fStatOnBeat = 0;
		fStatWaiting = fStatBeatWait = 0;
		reportFrameTimes();
		if (fDiff) {
			IOLog("MiniIOMobileFramebuffer: %llu pixels of the panel changed a frame, all within %u,%u to %u,%u\n",
			    fDiffPixels / fStatFrames, fDiffLeft, fDiffTop, fDiffRight, fDiffBottom);
			fDiffPixels = 0;
			fDiffLeft = fDiffTop = ~0u;
			fDiffRight = fDiffBottom = 0;
		}
		fStatFrames = 0;
		fStatDrawing = 0;
		fStatSince = 0;
	}
	IOLockUnlock(fLock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOMobileFramebufferUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

	task_t                  fTask;
	IOMobileFramebuffer *   fOwner;
	IOLock *                fLock;
	mach_port_t             fPorts[kFBNotifyTypes];
	mach_vm_address_t       fCallbacks[kFBNotifyTypes];
	io_user_reference_t     fRefcons[kFBNotifyTypes];
	thread_call_t           fVSyncCall;
	uint64_t                fVSyncCount;
	uint64_t                fVSyncTime;             /* when the refresh being waited for is due */
	bool                    fVSyncRunning;
	bool                    fClosed;

	static void vsyncFired(thread_call_param_t self, thread_call_param_t unused);
	void vsync(void);
	void armVSync(void);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService * provider)
{
	fOwner = OSDynamicCast(IOMobileFramebuffer, provider);
	if (fOwner == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fVSyncCall = thread_call_allocate(&IOMobileFramebufferUserClient::vsyncFired, this);
	return fLock != NULL && fVSyncCall != NULL;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fLock != NULL) {
		IOLockLock(fLock);
		fClosed = true;
		IOLockUnlock(fLock);
	}
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::free(void)
{
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
		thread_call_free(fVSyncCall);
		fVSyncCall = NULL;
	}
	for (unsigned i = 0; i < kFBNotifyTypes; i++) {
		if (fPorts[i] != NULL) {
			releaseNotificationPort(fPorts[i]);
			fPorts[i] = NULL;
		}
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	mach_port_t old;

	IOLog("MiniIOMobileFramebuffer: notification port for type %u\n", (unsigned) type);
	if (type >= kFBNotifyTypes) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	old = fPorts[type];
	fPorts[type] = port;
	IOLockUnlock(fLock);
	if (old != NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::vsyncFired(thread_call_param_t self, thread_call_param_t unused)
{
	((IOMobileFramebufferUserClient *) self)->vsync();
}

/* With the lock held. */
void
IOMobileFramebufferUserClient::armVSync(void)
{
	uint64_t period, now = mach_absolute_time();

	/* one refresh after the last, not after whenever we got round to this: the rate does not drift */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
	fVSyncTime += period;
	if (fVSyncTime <= now || fVSyncTime > now + 2 * period) {
		fVSyncTime = now + period;
	}
	thread_call_enter_delayed(fVSyncCall, fVSyncTime);
}

/*
 * One refresh: tell the client, which gets three 64-bit numbers as six 32-bit halves --
 * the time of this refresh, the same again (the time it was presented), and a counter.
 */
void
IOMobileFramebufferUserClient::vsync(void)
{
	OSAsyncReference64 ref;
	io_user_reference_t args[6];
	uint64_t now;
	bool send = false;

	IOLockLock(fLock);
	now = fVSyncTime;
	if (fClosed || fCallbacks[kFBNotifyVSync] == 0 || fPorts[kFBNotifyVSync] == NULL) {
		fVSyncRunning = false;
	} else {
		fVSyncCount++;
		setAsyncReference64(ref, fPorts[kFBNotifyVSync], fCallbacks[kFBNotifyVSync], fRefcons[kFBNotifyVSync], fTask);
		args[0] = (uint32_t) now;
		args[1] = now >> 32;
		args[2] = (uint32_t) now;
		args[3] = now >> 32;
		args[4] = (uint32_t) fVSyncCount;
		args[5] = fVSyncCount >> 32;
		send = true;
		armVSync();
	}
	IOLockUnlock(fLock);
	if (send) {
		sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, 6, kIOUserNotifyOptionCanDrop);
	}
}

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint64_t * in = args->scalarInput;
	uint64_t * out = args->scalarOutput;
	MiniIOSurface * s;

	if (selector < 128 && gFBCalls[selector] < gFBTrace) {
		char procname[32] = "?";

		gFBCalls[selector]++;
		proc_selfname(procname, sizeof(procname));
		IOLog("MiniIOMobileFramebuffer: %s[%d] selector %u (scalars %u in [0x%llx 0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", procname, proc_selfpid(), selector, args->scalarInputCount,
		    args->scalarInputCount > 0 ? in[0] : 0, args->scalarInputCount > 1 ? in[1] : 0,
		    args->scalarInputCount > 2 ? in[2] : 0, args->scalarInputCount > 3 ? in[3] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}

	switch (selector) {
	case kFBMethodGetDisplaySize:                   /* out: width, height */
		if (args->scalarOutputCount < 2) {
			return kIOReturnBadArgument;
		}
		out[0] = fOwner->fWidth;
		out[1] = fOwner->fHeight;
		args->scalarOutputCount = 2;
		return kIOReturnSuccess;

	case kFBMethodGetID:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetDotPitch:                      /* out: pixels per inch */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = kFBDotsPerInch * fOwner->fWidth / kFBDefaultWidth;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetLayerDefaultSurface:           /* in: size wanted (or 0, 0); out: a send right for the surface */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		s = fOwner->defaultSurface();
		if (s == NULL) {
			return kIOReturnNoMemory;
		}
		out[0] = MiniIOSurfaceMakeSendRight(fTask, s);
		args->scalarOutputCount = 1;
		return out[0] != 0 ? kIOReturnSuccess : kIOReturnNoMemory;

	case kFBMethodSwapBegin:                        /* out: the number of the new swap */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = OSIncrementAtomic(&fOwner->fSwapID) + 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodSwapEnd:
		if (args->structureInput == NULL || args->structureInputSize < SWAP_SIZE) {
			IOLog("MiniIOMobileFramebuffer: swap request of %u bytes, expected %u\n", args->structureInputSize, SWAP_SIZE);
			return kIOReturnBadArgument;
		}
		fOwner->present((const uint8_t *) args->structureInput);
		return kIOReturnSuccess;

	case kFBMethodSwapWait:                         /* a swap is on the screen as soon as it is made */
		if (gTouchLat) {
			redwood_tl("tl: w %llu %#llx %#llx at %llu\n", args->scalarInputCount > 0 ? in[0] : 0,
			    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, micros(mach_absolute_time()));
		}
		return kIOReturnSuccess;
	case kFBMethodSwapSignal:
	case kFBMethodSwapCancel:
	case kFBMethodSwapCancelAll:
	case kFBMethodRequestPowerChange:
	case kFBMethodChangeFrameInfo:
		return kIOReturnSuccess;

	case kFBMethodNotifications:                    /* in: callback (0 = off), its argument, type, 0 */
		if (args->scalarInputCount < 3 || in[2] >= kFBNotifyTypes) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		fCallbacks[in[2]] = in[0];
		fRefcons[in[2]] = in[1];
		if (in[2] == kFBNotifyVSync && in[0] != 0 && !fVSyncRunning && !fClosed) {
			fVSyncRunning = true;
			armVSync();
		}
		IOLockUnlock(fLock);
		return kIOReturnSuccess;

	case kFBMethodSupportedFrameInfo:               /* in: type; out: how many, and their tags: none */
		if (args->scalarOutputCount >= 1) {
			out[0] = 0;
			args->scalarOutputCount = 1;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		return kIOReturnSuccess;

	case kFBMethodGetBlock:                         /* in: which block; out: its bytes */
		if (args->structureOutput == NULL) {
			return kIOReturnBadArgument;
		}
		bzero(args->structureOutput, args->structureOutputSize);
		if (args->scalarInputCount >= 1 && in[0] == 0x40 && args->structureOutputSize >= 12) {
			/* the current mode: its colour and timing identifiers */
			uint32_t ids[2] = { kFBColorID, kFBTimingID };

			memcpy((uint8_t *) args->structureOutput + 4, ids, sizeof(ids));
		}
		return kIOReturnSuccess;

	case kFBMethodGetCurrentAbsoluteTime:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = mach_absolute_time();
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 128 && !(gFBLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gFBLogged[selector / 64] |= 1ULL << (selector % 64);
		IOLog("MiniIOMobileFramebuffer: selector %u not implemented (scalars %u in [0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", selector, args->scalarInputCount, args->scalarInputCount > 0 ? in[0] : 0,
		    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}
	/* a request that only sets something is accepted; one that wants an answer gets zeros */
	if (args->scalarOutputCount != 0) {
		bzero(out, args->scalarOutputCount * sizeof(out[0]));
	}
	if (args->structureOutput != NULL && args->structureOutputSize != 0) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * IOMobileFramebuffer.framework counts the displays it should wait for by looking for
 * children of /arm-io in the device tree whose device_type is "display-subsystem".
 */
static void
publishDisplayNode(void)
{
	IORegistryEntry * armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
	IORegistryEntry * node;
	OSDictionary * props;
	OSData * data;

	if (armio == NULL) {
		IOLog("MiniIOMobileFramebuffer: no /arm-io in the device tree\n");
		return;
	}
	node = OSTypeAlloc(IORegistryEntry);
	props = OSDictionary::withCapacity(4);
	if (node != NULL && props != NULL) {
		data = OSData::withBytes("display-subsystem", sizeof("display-subsystem"));
		props->setObject("device_type", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("redwood,framebuffer", sizeof("redwood,framebuffer"));
		props->setObject("compatible", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("disp0", sizeof("disp0"));
		props->setObject("name", data);
		OSSafeReleaseNULL(data);
		if (node->init(props)) {
			node->setName("disp0");
			node->attachToParent(armio, gIODTPlane);
		}
	}
	OSSafeReleaseNULL(props);
	OSSafeReleaseNULL(node);
	armio->release();
}

/*
 * The graphics accelerator. There is none, but CoreAnimation waits half a minute for a
 * service of this class to appear before it looks for a GPU at all (once per process). An
 * accelerator without the properties that name its Metal and OpenGL ES drivers is passed
 * over by both, which leaves CoreAnimation with its software renderer, as before.
 *
 * With boot-arg metal=1 a user process may put those two properties here, which is the whole
 * of what keeps Metal from finding our driver; see setProperties below, and docs/metal.md 10.31
 * (10.14 for the measurement that made it a boot-arg).
 */

/* Metal's step 2 reads exactly these two, in this order, off this service (docs/metal.md 1). */
#define kAcceleratorPluginNameKey      "MetalPluginName"
#define kAcceleratorPluginClassNameKey "MetalPluginClassName"

static bool gAcceleratorTakesProperties;        /* boot-arg metal=1 */

class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);

public:
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

/*
 * Switch Metal's own device discovery on, from user space: IORegistryEntrySetCFProperties() with
 * MetalPluginName and MetalPluginClassName. Metal reads the first (getMetalPluginClassForService,
 * 0x181a39ba4), builds "/System/Library/Extensions/" + it + ".bundle", opens that with NSBundle
 * and takes the class the second names; with the first missing it returns nil before it even
 * reads the second, and that single `cbz` is where the chain breaks on this port today.
 *
 * This method is the only thing that was in the way. IORegistryEntry::setProperties is
 * "return kIOReturnUnsupported" (IORegistryEntry.cpp) and IOService does not override it; the
 * MAC check in front of the call has no policy to ask, because this kernel registers no iokit
 * hook (bsd/kern/kern_minimac.c); and this service carries no property whitelist.
 *
 * Why it is gated on a boot-arg, which is a measured risk and not caution in general
 * (docs/metal.md 10.14): the conditions Metal puts on a device object are asserts, not soft
 * refusals (10.2-10.3). A process that asks Metal and is handed a half-made device does not
 * degrade to another renderer -- it aborts ("Assertion failed: ([self
 * conformsToProtocol:@protocol(MTLDeviceSPI)])", "failed assertion _limits.maxColorAttachments
 * > 0"), and an aborting daemon is restarted by launchd, which is a loop. Published instead at
 * registerService() time, the properties would exist before launchd, backboardd and SpringBoard
 * are born, so each of the three would take the Metal road on its first call, with nobody left
 * to read the log. Behind a boot-arg, a machine that has them is a machine booted on purpose to
 * have them, and the first such boot is meant to be RC=rc.ios, where launchd is never born.
 *
 * An empty string removes a property rather than setting it. That is the way out of the loop
 * above without a reboot: the properties live only in the registry, so processes started after
 * they are gone go back to the software road.
 *
 * Everything else is refused, and refused whole, before anything is set: every process on the
 * machine can reach this method and the only thing it exists for is this one switch.
 */
IOReturn
IOAcceleratorES::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSCollectionIterator * keys;
	OSObject * next;
	char who[32] = "?";

	if (!gAcceleratorTakesProperties) {
		static unsigned said;

		if (said++ < 4) {
			proc_selfname(who, sizeof(who));
			IOLog("IOAcceleratorES: %s asks to set properties on it; boot-arg metal=1 was not given\n", who);
		}
		return kIOReturnUnsupported;
	}
	if (dict == NULL || dict->getCount() == 0) {
		return kIOReturnBadArgument;
	}
	keys = OSCollectionIterator::withCollection(dict);
	if (keys == NULL) {
		return kIOReturnNoMemory;
	}
	proc_selfname(who, sizeof(who));
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);

		if (key == NULL || (!key->isEqualTo(kAcceleratorPluginNameKey) &&
		    !key->isEqualTo(kAcceleratorPluginClassNameKey))) {
			IOLog("IOAcceleratorES: refusing what %s sent: only " kAcceleratorPluginNameKey " and "
			    kAcceleratorPluginClassNameKey " are taken here\n", who);
			keys->release();
			return kIOReturnUnsupported;
		}
		if (OSDynamicCast(OSString, dict->getObject(key)) == NULL) {
			IOLog("IOAcceleratorES: refusing \"%s\" from %s: Metal reads it as a CFString\n",
			    key->getCStringNoCopy(), who);
			keys->release();
			return kIOReturnBadArgument;
		}
	}
	keys->reset();
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);
		OSString * value = OSDynamicCast(OSString, dict->getObject(key));

		if (value->getLength() == 0) {
			removeProperty(key);
			IOLog("IOAcceleratorES: %s removed by %s\n", key->getCStringNoCopy(), who);
		} else if (setProperty(key, value)) {
			IOLog("IOAcceleratorES: %s = %s, set by %s\n", key->getCStringNoCopy(),
			    value->getCStringNoCopy(), who);
		} else {
			IOLog("IOAcceleratorES: %s could not be set\n", key->getCStringNoCopy());
			keys->release();
			return kIOReturnNoMemory;
		}
	}
	keys->release();
	return kIOReturnSuccess;
}

static void
publishAccelerator(IOService * provider)
{
	IOAcceleratorES * accelerator = OSTypeAlloc(IOAcceleratorES);
	uint32_t metal = 0;

	gAcceleratorTakesProperties = PE_parse_boot_argn("metal", &metal, sizeof(metal)) && metal != 0;
	if (accelerator != NULL && accelerator->init() && accelerator->attach(provider)) {
		accelerator->setName("sgx");
		accelerator->registerService();
		/* the first line to look for in the log of such a boot: the gate is open, nothing is set yet */
		if (gAcceleratorTakesProperties) {
			IOLog("IOAcceleratorES: metal=%u: this service takes " kAcceleratorPluginNameKey
			    " and " kAcceleratorPluginClassNameKey " from user space\n", metal);
		}
	}
	OSSafeReleaseNULL(accelerator);
}

/*
 * A picture from inside the kernel (the GPU driver's first ones): rows of 32-bit pixels, put at
 * the top left corner of the panel as they are and cut to its size.
 */
extern "C" void
MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	uint32_t columns = width < vinfo.v_width ? width : (uint32_t) vinfo.v_width;
	uint32_t rows = height < vinfo.v_height ? height : (uint32_t) vinfo.v_height;

	if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) {
		return;
	}
	for (uint32_t y = 0; y < rows; y++) {
		memcpy((void *) (vinfo.v_baseaddr + (size_t) y * vinfo.v_rowbytes), pixels + (size_t) y * rowBytes,
		    (size_t) columns * 4);
	}
}

void
MiniIOMobileFramebufferStart(IOService * provider)
{
	IOMobileFramebuffer * fb = OSTypeAlloc(IOMobileFramebuffer);

	if (fb == NULL || !fb->init() || !fb->attach(provider)) {
		OSSafeReleaseNULL(fb);
		return;
	}
	if (fb->start(provider)) {
		publishDisplayNode();
		publishAccelerator(provider);
	} else {
		fb->detach(provider);
	}
	fb->release();
}
