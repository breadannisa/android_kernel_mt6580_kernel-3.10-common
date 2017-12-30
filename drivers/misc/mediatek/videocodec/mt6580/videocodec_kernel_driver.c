#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/mm_types.h>
#include <linux/mm.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <asm/uaccess.h>
#include <asm/page.h>
#include <linux/vmalloc.h>
#include <linux/interrupt.h>
#include <mach/irqs.h>
//#include <mach/x_define_irq.h>
#include <linux/wait.h>
#include <linux/proc_fs.h>
#include <linux/semaphore.h>
#include <mach/dma.h>
#include <linux/delay.h>
#include <linux/earlysuspend.h>
#include "mach/sync_write.h"
//#include "mach/mt_reg_base.h"
#include "mach/mt_clkmgr.h"

#ifdef CONFIG_MTK_HIBERNATION
#include "mach/mtk_hibernate_dpm.h"
#endif

#include "videocodec_kernel_driver.h"
#include <asm/cacheflush.h>
#include <asm/io.h>
#include <asm/sizes.h>
#include "val_types_private.h"
#include "hal_types_private.h"
#include "val_api_private.h"
#include "val_log.h"
#include "drv_api.h"

#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#if IS_ENABLED(CONFIG_COMPAT)
#include <linux/uaccess.h>
#include <linux/compat.h>
#endif

#define CONFIG_ARCH_MT6580
#ifdef CONFIG_ARCH_MT6580
#include "mt_irq.h"
#endif

#include <mt_smi.h>
//#define ENABLE_MMDVFS_VDEC
#ifdef ENABLE_MMDVFS_VDEC
// <--- MM DVFS related
#define DROP_PERCENTAGE     50
#define RAISE_PERCENTAGE    90
#define MONITOR_DURATION_MS 4000
#define DVFS_LOW     MMDVFS_VOLTAGE_LOW
#define DVFS_HIGH    MMDVFS_VOLTAGE_HIGH
#define DVFS_DEFAULT MMDVFS_VOLTAGE_HIGH
#define MONITOR_START_MINUS_1   0
#define SW_OVERHEAD_MS 1
static VAL_BOOL_T   gMMDFVFSMonitorStarts = VAL_FALSE;
static VAL_BOOL_T   gFirstDvfsLock = VAL_FALSE;
static VAL_UINT32_T gMMDFVFSMonitorCounts = 0;
static VAL_TIME_T   gMMDFVFSMonitorStartTime;
static VAL_TIME_T   gMMDFVFSLastLockTime;
static VAL_TIME_T   gMMDFVFSMonitorEndTime;
static VAL_UINT32_T gHWLockInterval = 0;
static VAL_INT32_T  gHWLockMaxDuration = 0;

VAL_UINT32_T TimeDiffMs(VAL_TIME_T timeOld, VAL_TIME_T timeNew)
{
    //MFV_LOGE ("@@ timeOld(%d, %d), timeNew(%d, %d)", timeOld.u4Sec, timeOld.u4uSec, timeNew.u4Sec, timeNew.u4uSec);
    return (((((timeNew.u4Sec - timeOld.u4Sec) * 1000000) + timeNew.u4uSec) - timeOld.u4uSec) / 1000);
}

// raise/drop voltage
void SendDvfsRequest(int level)
{
    int ret = 0;

    if (level == MMDVFS_VOLTAGE_LOW)
    {
        clkmux_sel(MT_MUX_VDEC, 3, "MMDVFS_VOLTAGE_LOW");   // 156MHz
        ret = mmdvfs_set_step(SMI_BWC_SCEN_VP, MMDVFS_VOLTAGE_LOW);
    }
    else if (level == MMDVFS_VOLTAGE_HIGH)
    {
        ret = mmdvfs_set_step(SMI_BWC_SCEN_VP, MMDVFS_VOLTAGE_HIGH);
        clkmux_sel(MT_MUX_VDEC, 4, "MMDVFS_VOLTAGE_HIGH");  // 273MHz
    }
    else
    {
        MFV_LOGE("[VCODEC][MMDVFS_VDEC] @@ OOPS: level = %d\n", level);
    }

    if (0 != ret)
    {
        MFV_LOGE("[VCODEC][MMDVFS_VDEC] @@ OOPS: mmdvfs_set_step error!");
    }
}

void VdecDvfsBegin(void)
{
    gMMDFVFSMonitorStarts = VAL_TRUE;
    gMMDFVFSMonitorCounts = 0;
    gHWLockInterval = 0;
    gFirstDvfsLock = VAL_TRUE;
    gHWLockMaxDuration = 0;
    MFV_LOGE("[VCODEC][MMDVFS_VDEC] @@ VdecDvfsBegin");
    //eVideoGetTimeOfDay(&gMMDFVFSMonitorStartTime, sizeof(VAL_TIME_T));
}

VAL_UINT32_T VdecDvfsGetMonitorDuration(void)
{
    eVideoGetTimeOfDay(&gMMDFVFSMonitorEndTime, sizeof(VAL_TIME_T));
    return TimeDiffMs(gMMDFVFSMonitorStartTime, gMMDFVFSMonitorEndTime);
}

void VdecDvfsEnd(int level)
{
    MFV_LOGE("[VCODEC][MMDVFS_VDEC] @@ VdecDVFS monitor %dms, decoded %d frames, total time %d, max duration %d, target lv %d",
             MONITOR_DURATION_MS, gMMDFVFSMonitorCounts, gHWLockInterval, gHWLockMaxDuration, level);
    gMMDFVFSMonitorStarts = VAL_FALSE;
    gMMDFVFSMonitorCounts = 0;
    gHWLockInterval = 0;
    gHWLockMaxDuration = 0;
}

VAL_UINT32_T VdecDvfsStep(void)
{
    VAL_TIME_T _now;
    VAL_UINT32_T _diff = 0;
    eVideoGetTimeOfDay(&_now, sizeof(VAL_TIME_T));
    _diff = TimeDiffMs(gMMDFVFSLastLockTime, _now);
    if (_diff > gHWLockMaxDuration)
    {
        gHWLockMaxDuration = _diff;
    }
    gHWLockInterval += (_diff + SW_OVERHEAD_MS);
    return _diff;
}
// --->
#endif

#define VDO_HW_WRITE(ptr,data)     mt_reg_sync_writel(data,ptr)
#define VDO_HW_READ(ptr)           (*((volatile unsigned int * const)(ptr)))

#define VCODEC_DEVNAME     "Vcodec"
#define VCODEC_DEV_MAJOR_NUMBER 160   //189
//#define VENC_USE_L2C

static dev_t vcodec_devno = MKDEV(VCODEC_DEV_MAJOR_NUMBER, 0);
static struct cdev *vcodec_cdev;
static struct class *vcodec_class = NULL;
static struct device *vcodec_device = NULL;

static DEFINE_MUTEX(IsOpenedLock);
static DEFINE_MUTEX(PWRLock);
//static DEFINE_MUTEX(VdecHWLock);
//static DEFINE_MUTEX(VencHWLock);
//static DEFINE_MUTEX(HWLock);
static DEFINE_MUTEX(EncEMILock);
static DEFINE_MUTEX(L2CLock);
static DEFINE_MUTEX(DecEMILock);
static DEFINE_MUTEX(DriverOpenCountLock);
static DEFINE_MUTEX(NonCacheMemoryListLock);
//static DEFINE_MUTEX(DecHWLockEventTimeoutLock);
//static DEFINE_MUTEX(EncHWLockEventTimeoutLock);
static DEFINE_MUTEX(HWLockEventTimeoutLock);

//static DEFINE_MUTEX(VdecPWRLock);
//static DEFINE_MUTEX(VencPWRLock);
static DEFINE_MUTEX(InitHWLock);

static DEFINE_SPINLOCK(OalHWContextLock);
static DEFINE_SPINLOCK(DecIsrLock);
static DEFINE_SPINLOCK(EncIsrLock);
static DEFINE_SPINLOCK(LockDecHWCountLock);
static DEFINE_SPINLOCK(LockEncHWCountLock);
static DEFINE_SPINLOCK(DecISRCountLock);
static DEFINE_SPINLOCK(EncISRCountLock);
static DEFINE_SPINLOCK(HWLock);
static DEFINE_SPINLOCK(VdecPWRLock);
static DEFINE_SPINLOCK(VencPWRLock);


//static VAL_EVENT_T DecHWLockEvent;    //mutex : HWLockEventTimeoutLock
//static VAL_EVENT_T EncHWLockEvent;    //mutex : HWLockEventTimeoutLock
static VAL_EVENT_T HWLockEvent;    //mutex : HWLockEventTimeoutLock
static VAL_EVENT_T DecIsrEvent;    //mutex : HWLockEventTimeoutLock
static VAL_EVENT_T EncIsrEvent;    //mutex : HWLockEventTimeoutLock
static VAL_INT32_T Driver_Open_Count;         //mutex : DriverOpenCountLock
static VAL_UINT32_T gu4PWRCounter = 0;      //mutex : PWRLock
static VAL_UINT32_T gu4EncEMICounter = 0;   //mutex : EncEMILock
static VAL_UINT32_T gu4DecEMICounter = 0;   //mutex : DecEMILock
static VAL_UINT32_T gu4L2CCounter = 0;      //mutex : L2CLock
static VAL_BOOL_T   bIsOpened = VAL_FALSE;    //mutex : IsOpenedLock
static VAL_UINT32_T gu4HwVencIrqStatus = 0; //hardware VENC IRQ status (VP8/H264)

static VAL_UINT32_T gu4VdecPWRCounter = 0;  //mutex : VdecPWRLock
static VAL_UINT32_T gu4VencPWRCounter = 0;  //mutex : VencPWRLock
static VAL_INT32_T gu4HybPWRCounter = 0;      //no mutex, just for debug

static VAL_UINT32_T gLockTimeOutCount = 0;

static VAL_UINT32_T gu4VdecLockThreadId = 0;

#if IS_ENABLED(CONFIG_COMPAT)
static VAL_UINT8_T *ori_user_data_addr = 0;
static VAL_VCODEC_OAL_MEM_STAUTS_T *ori_pHWStatus = 0;
#endif

//#define VCODEC_DEBUG
#ifdef VCODEC_DEBUG
#undef VCODEC_DEBUG
#define VCODEC_DEBUG MFV_LOGE
#undef MFV_LOGD
#define MFV_LOGD  MFV_LOGE
#else
#define VCODEC_DEBUG(...)
#undef MFV_LOGD
#define MFV_LOGD(...)
#endif

// VENC physical base address
#undef VENC_BASE
#define VENC_BASE       0x17002000
#define VENC_REGION     0x1000

#undef MP4_VENC_BASE
#define MP4_VENC_BASE       0x15009000
#define MP4_VENC_REGION     0x1000

// VDEC virtual base address
#define VDEC_BASE_PHY   0x1500B000
#define VDEC_REGION     0x2000

#define HW_BASE         0x7FFF000
#define HW_REGION       0x2000

#define INFO_BASE       0x10000000
#define INFO_REGION     0x1000

#if 0
#define VENC_IRQ_STATUS_addr        VENC_BASE + 0x05C
#define VENC_IRQ_ACK_addr           VENC_BASE + 0x060
#define VENC_MP4_IRQ_ACK_addr       VENC_BASE + 0x678
#define VENC_MP4_IRQ_STATUS_addr    VENC_BASE + 0x67C
#define VENC_ZERO_COEF_COUNT_addr   VENC_BASE + 0x688
#define VENC_BYTE_COUNT_addr        VENC_BASE + 0x680
#define VENC_MP4_IRQ_ENABLE_addr    VENC_BASE + 0x668

#define VENC_MP4_STATUS_addr        VENC_BASE + 0x664
#define VENC_MP4_MVQP_STATUS_addr   VENC_BASE + 0x6E4
#endif


#define VENC_IRQ_STATUS_SPS         0x1
#define VENC_IRQ_STATUS_PPS         0x2
#define VENC_IRQ_STATUS_FRM         0x4
#define VENC_IRQ_STATUS_DRAM        0x8
#define VENC_IRQ_STATUS_PAUSE       0x10
#define VENC_IRQ_STATUS_SWITCH      0x20

//#define VENC_PWR_FPGA
// Cheng-Jung 20120621 VENC power physical base address (FPGA only, should use API) [
#ifdef VENC_PWR_FPGA
#define CLK_CFG_0_addr      0x10000140
#define CLK_CFG_4_addr      0x10000150
#define VENC_PWR_addr       0x10006230
#define VENCSYS_CG_SET_addr 0x15000004

#define PWR_ONS_1_D     3
#define PWR_CKD_1_D     4
#define PWR_ONN_1_D     2
#define PWR_ISO_1_D     1
#define PWR_RST_0_D     0

#define PWR_ON_SEQ_0    ((0x1 << PWR_ONS_1_D) | (0x1 << PWR_CKD_1_D) | (0x1 << PWR_ONN_1_D) | (0x1 << PWR_ISO_1_D) | (0x0 << PWR_RST_0_D))
#define PWR_ON_SEQ_1    ((0x1 << PWR_ONS_1_D) | (0x0 << PWR_CKD_1_D) | (0x1 << PWR_ONN_1_D) | (0x1 << PWR_ISO_1_D) | (0x0 << PWR_RST_0_D))
#define PWR_ON_SEQ_2    ((0x1 << PWR_ONS_1_D) | (0x0 << PWR_CKD_1_D) | (0x1 << PWR_ONN_1_D) | (0x0 << PWR_ISO_1_D) | (0x0 << PWR_RST_0_D))
#define PWR_ON_SEQ_3    ((0x1 << PWR_ONS_1_D) | (0x0 << PWR_CKD_1_D) | (0x1 << PWR_ONN_1_D) | (0x0 << PWR_ISO_1_D) | (0x1 << PWR_RST_0_D))
// ]
#endif

#if 0
// VDEC virtual base address
#define VDEC_MISC_BASE  VDEC_BASE + 0x0000
#define VDEC_VLD_BASE   VDEC_BASE + 0x1000
#endif


VAL_ULONG_T KVA_VENC_IRQ_ACK_ADDR, KVA_VENC_IRQ_STATUS_ADDR, KVA_VENC_BASE;
VAL_ULONG_T KVA_VENC_ZERO_COEF_COUNT_ADDR, KVA_VENC_BYTE_COUNT_ADDR; //hybrid
VAL_ULONG_T KVA_VDEC_MISC_BASE, KVA_VDEC_VLD_BASE, KVA_VDEC_BASE, KVA_VDEC_GCON_BASE;
VAL_ULONG_T KVA_VDEC_INT_STA_ADDR, KVA_VDEC_INT_ACK_ADDR; //hybrid
VAL_UINT32_T VENC_IRQ_ID, VDEC_IRQ_ID;
VAL_ULONG_T KVA_VENC_MP4_IRQ_ENABLE_ADDR;


#ifdef VENC_PWR_FPGA
// Cheng-Jung 20120621 VENC power physical base address (FPGA only, should use API) [
VAL_ULONG_T KVA_VENC_CLK_CFG_0_ADDR, KVA_VENC_CLK_CFG_4_ADDR, KVA_VENC_PWR_ADDR, KVA_VENCSYS_CG_SET_ADDR;
// ]
#endif

extern unsigned long pmem_user_v2p_video(unsigned long va);

#if defined(VENC_USE_L2C)
extern int config_L2(int option);
#endif

void vdec_power_on(void)
{
    VAL_ULONG_T ulFlagsVdecPWRLock;
    spin_lock_irqsave(&VdecPWRLock, ulFlagsVdecPWRLock);
    gu4VdecPWRCounter++;
    spin_unlock_irqrestore(&VdecPWRLock, ulFlagsVdecPWRLock);
    ++gu4HybPWRCounter;

    // Central power on
    MFV_LOGE("[VCODEC] vdec_power_on + (%d) (%d) (%d)\n", gu4VdecPWRCounter, gu4HybPWRCounter, gu4VDecIRQCount);
    enable_clock(MT_CG_DISP0_SMI_COMMON, "VDEC");
    enable_clock(MT_CG_VCODEC_CKPDN, "VDEC");
    enable_clock(MT_CG_LARB1_SMI_CKPDN , "VDEC");
#ifdef VDEC_USE_L2C
    //enable_clock(MT_CG_INFRA_L2C_SRAM, "VDEC");
#endif
    MFV_LOGD("[VCODEC] vdec_power_on -\n");
}

void vdec_power_off(void)
{
    VAL_ULONG_T ulFlagsVdecPWRLock;
    spin_lock_irqsave(&VdecPWRLock, ulFlagsVdecPWRLock);
    if (gu4VdecPWRCounter == 0)
    {
    }
    else
    {
        gu4VdecPWRCounter--;
        --gu4HybPWRCounter;
        // Central power off
        MFV_LOGE("[VCODEC] vdec_power_off + (%d) (%d) (%d)\n", gu4VdecPWRCounter, gu4HybPWRCounter, gu4VDecIRQCount);
        disable_clock(MT_CG_LARB1_SMI_CKPDN, "VDEC");
        disable_clock(MT_CG_VCODEC_CKPDN, "VDEC");
        disable_clock(MT_CG_DISP0_SMI_COMMON, "VDEC");
#ifdef VDEC_USE_L2C
        //disable_clock(MT_CG_INFRA_L2C_SRAM, "VDEC");
#endif
        MFV_LOGD("[VCODEC] vdec_power_off -\n");
    }
    spin_unlock_irqrestore(&VdecPWRLock, ulFlagsVdecPWRLock);
}

void venc_power_on(void)
{
    VAL_ULONG_T ulFlagsVencPWRLock;
    spin_lock_irqsave(&VencPWRLock, ulFlagsVencPWRLock);
    gu4VencPWRCounter++;
    spin_unlock_irqrestore(&VencPWRLock, ulFlagsVencPWRLock);
    ++gu4HybPWRCounter;

    MFV_LOGE("[VCODEC] venc_power_on + (%d) (%d) (%d)\n", gu4VencPWRCounter, gu4HybPWRCounter, gu4VEncIRQCount);
    enable_clock(MT_CG_DISP0_SMI_COMMON, "VENC");
    enable_clock(MT_CG_VCODEC_CKPDN, "VENC");
    enable_clock(MT_CG_LARB1_SMI_CKPDN , "VENC");

#ifdef VENC_USE_L2C
    enable_clock(MT_CG_INFRA_L2C_SRAM, "VENC");
#endif
    //enable_clock(MT_CG_MM_CODEC_SW_CG, "VideoClock");
    //larb_clock_on(0, "VideoClock");
    VDO_HW_WRITE(KVA_VENC_MP4_IRQ_ENABLE_ADDR, 0x1);
    MFV_LOGD("[VCODEC] venc_power_on -\n");
}

void venc_power_off(void)
{
    VAL_ULONG_T ulFlagsVencPWRLock;
    spin_lock_irqsave(&VencPWRLock, ulFlagsVencPWRLock);
    if (gu4VencPWRCounter == 0)
    {
    }
    else
    {
        gu4VencPWRCounter--;
        --gu4HybPWRCounter;
        MFV_LOGE("[VCODEC] venc_power_off + (%d) (%d) (%d)\n", gu4VencPWRCounter, gu4HybPWRCounter, gu4VEncIRQCount);
        disable_clock(MT_CG_LARB1_SMI_CKPDN, "VENC");
        disable_clock(MT_CG_VCODEC_CKPDN, "VENC");
        disable_clock(MT_CG_DISP0_SMI_COMMON, "VENC");
#ifdef VENC_USE_L2C
        disable_clock(MT_CG_INFRA_L2C_SRAM, "VENC");
#endif
        //disable_clock(MT_CG_MM_CODEC_SW_CG, "VideoClock");
        //larb_clock_off(0, "VideoClock");
        MFV_LOGD("[VCODEC] venc_power_off -\n");
    }
    spin_unlock_irqrestore(&VencPWRLock, ulFlagsVencPWRLock);
}

void dec_isr(void)
{
    VAL_RESULT_T    eValRet;
    VAL_ULONG_T     ulFlags, ulFlagsISR, ulFlagsLockHW, ulFlagsHWLock;

    VAL_UINT32_T u4TempDecISRCount = 0;
    VAL_UINT32_T u4TempLockDecHWCount = 0;
    VAL_UINT32_T u4CgStatus = 0;
    VAL_UINT32_T u4DecDoneStatus = 0;
    unsigned int reg_val;
    VAL_UINT32_T index, i, maxnum;
    VAL_UINT32_T u4IRQStatus = 0;
    VAL_RESULT_T  eValHWLockRet = VAL_RESULT_INVALID_ISR;

    if (grVcodecHWLock.eDriverType != VAL_DRIVER_TYPE_H264_DEC && grVcodecHWLock.eDriverType != VAL_DRIVER_TYPE_VP8_DEC)
    {
        //u4CgStatus = VDO_HW_READ(KVA_VDEC_GCON_BASE);
        //if ((u4CgStatus & 0x10) != 0)
        {
            //MFV_LOGE("[VCODEC][ERROR] DEC ISR, VDEC active is not 0x0 (0x%08x)", u4CgStatus);
            //return;
        }

        u4DecDoneStatus = VDO_HW_READ(KVA_VDEC_BASE + 0xA4);
        if ((u4DecDoneStatus & (0x1 << 16)) != 0x10000)
        {
            MFV_LOGE("[VCODEC][ERROR] DEC ISR, Decode done status is not 0x1 (0x%08x)", u4DecDoneStatus);
            return;
        }
    }


    spin_lock_irqsave(&DecISRCountLock, ulFlagsISR);
    gu4DecISRCount++;
    u4TempDecISRCount = gu4DecISRCount;
    spin_unlock_irqrestore(&DecISRCountLock, ulFlagsISR);

    spin_lock_irqsave(&LockDecHWCountLock, ulFlagsLockHW);
    u4TempLockDecHWCount = gu4LockDecHWCount;
    spin_unlock_irqrestore(&LockDecHWCountLock, ulFlagsLockHW);

    if (u4TempDecISRCount != u4TempLockDecHWCount)
    {
        //MFV_LOGE("[INFO] Dec ISRCount: 0x%x, LockHWCount:0x%x\n", u4TempDecISRCount, u4TempLockDecHWCount);
    }

    if (grVcodecHWLock.eDriverType == VAL_DRIVER_TYPE_H264_DEC ||
            grVcodecHWLock.eDriverType == VAL_DRIVER_TYPE_VP8_DEC)
    {
        spin_lock_irqsave(&OalHWContextLock, ulFlags);
        index = search_HWLockSlot_ByHandle(0, (VAL_HANDLE_T)grVcodecHWLock.pvHandle);

        //MFV_LOGD("index = %d\n", index);

        // in case, if the process is killed first,
        // then receive an ISR from HW, the event information already cleared.
        if (index == -1) // Hybrid
        {
            MFV_LOGE("[VCODEC][ERROR][ISR] Can't find any index in ISR\n");

            // ACK interrupt
            // decoder
            reg_val = VDO_HW_READ(KVA_VDEC_INT_STA_ADDR);
            VDO_HW_WRITE(KVA_VDEC_INT_ACK_ADDR, reg_val);
            spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

            return;
        }

        // get address from context
        //MFV_LOGD("ISR: Total %d u4NumOfRegister\n", oal_hw_context[index].u4NumOfRegister);

        maxnum = oal_hw_context[index].u4NumOfRegister;
        if (oal_hw_context[index].u4NumOfRegister > VCODEC_MULTIPLE_INSTANCE_NUM)
        {
            MFV_LOGE("[VCODEC][ERROR][ISR] oal_hw_context[index].u4NumOfRegister =%d\n",
                    oal_hw_context[index].u4NumOfRegister);
            maxnum = VCODEC_MULTIPLE_INSTANCE_NUM;
        }

        //MFV_LOGD("oal_hw_context[index].kva_u4HWIsCompleted 0x%x value=%d \n",
        //oal_hw_context[index].kva_u4HWIsCompleted,
        //*((volatile VAL_UINT32_T*)oal_hw_context[index].kva_u4HWIsCompleted));
        if ((((volatile VAL_UINT32_T *)oal_hw_context[index].kva_u4HWIsCompleted) == NULL) ||
                (((volatile VAL_UINT32_T *)oal_hw_context[index].kva_u4HWIsTimeout) == NULL))
        {
            MFV_LOGE("[VCODEC][ERROR][ISR] index = %d, please check!!\n", index);

            // ACK interrupt
            // decoder
            reg_val = VDO_HW_READ(KVA_VDEC_INT_STA_ADDR);
            VDO_HW_WRITE(KVA_VDEC_INT_ACK_ADDR, reg_val);
            spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

            return;
        }
        *((volatile VAL_UINT32_T *)oal_hw_context[index].kva_u4HWIsCompleted) = 1;
        *((volatile VAL_UINT32_T *)oal_hw_context[index].kva_u4HWIsTimeout) = 0;

        for (i = 0; i < maxnum ; i++)
        {
            //MFV_LOGD("[BEFORE] ISR read: [%d]  User_va=0x%x kva=0x%x 0x%x \n", i ,
            //*((volatile VAL_UINT32_T*)oal_hw_context[index].kva_Oal_HW_mem_reg + i*2),
            //oal_hw_context[index].oalmem_status[i].u4ReadAddr,
            //*((volatile VAL_UINT32_T*)oal_hw_context[index].kva_Oal_HW_mem_reg + i*2 + 1));

            *((volatile VAL_UINT32_T *)oal_hw_context[index].kva_Oal_HW_mem_reg + i * 2 + 1) = 
                *((volatile VAL_UINT32_T *)oal_hw_context[index].oalmem_status[i].u4ReadAddr);

            if (maxnum == 3)
            {
                if (i == 0)
                {
                    u4IRQStatus = (*((volatile VAL_UINT32_T *)oal_hw_context[index].kva_Oal_HW_mem_reg + i * 2 + 1));
                    if (u4IRQStatus != 2)
                    {
                        MFV_LOGE("[VCODEC][ERROR][ISR] IRQ status error u4IRQStatus = %d\n", u4IRQStatus);
                    }
                }

                if (u4IRQStatus != 2)
                {
                    MFV_LOGE("[VCODEC][ERROR][ISR] %d, 0x%lx, %d, %d, %d, %d\n",
                            i,
                            (VAL_ULONG_T)((volatile VAL_UINT32_T *)oal_hw_context[index].oalmem_status[i].u4ReadAddr),
                            (*((volatile VAL_UINT32_T *)oal_hw_context[index].kva_Oal_HW_mem_reg + i * 2 + 1)),
                            VDO_HW_READ(KVA_VENC_IRQ_ACK_ADDR),
                            VDO_HW_READ(KVA_VENC_ZERO_COEF_COUNT_ADDR),
                            VDO_HW_READ(KVA_VENC_BYTE_COUNT_ADDR));
                }
            }

            //MFV_LOGD("[AFTER] ISR read: [%d]  User_va=0x%x kva=0x%x 0x%x \n", i ,
            //*((volatile VAL_UINT32_T*)oal_hw_context[index].kva_Oal_HW_mem_reg + i*2),
            //oal_hw_context[index].oalmem_status[i].u4ReadAddr,
            //*((volatile VAL_UINT32_T*)oal_hw_context[index].kva_Oal_HW_mem_reg + i*2 + 1) /*oal_hw_context[index].oalmem_status[i].u4ReadData*/);
        }
        //set handle event
        eValRet = eVideoSetEvent(&oal_hw_context[index].IsrEvent, sizeof(VAL_EVENT_T));
        spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

        if (VAL_RESULT_NO_ERROR != eValRet)
        {
            MFV_LOGE("[VCODEC][ERROR] ISR set IsrEvent error\n");
            //return;
        }

        // Clear interrupt
        // ACK interrupt
        // decoder
        reg_val = VDO_HW_READ(KVA_VDEC_INT_STA_ADDR);
        VDO_HW_WRITE(KVA_VDEC_INT_ACK_ADDR, reg_val);

        //Unlock
        spin_lock_irqsave(&HWLock, ulFlagsHWLock);
        for (i = 0; i < VCODEC_THREAD_MAX_NUM; i++)
        {
            grVcodecHWLock.u4VCodecThreadID[i] = -1;
        }
        grVcodecHWLock.u4VCodecThreadNum = VCODEC_THREAD_MAX_NUM;
        grVcodecHWLock.rLockedTime.u4Sec = 0;
        grVcodecHWLock.rLockedTime.u4uSec = 0;
        grVcodecHWLock.pvHandle = 0;
        disable_irq_nosync(VDEC_IRQ_ID);
        --gu4VDecIRQCount;
        // turn vdec power off
        vdec_power_off();

        eValHWLockRet = eVideoSetEvent(&HWLockEvent, sizeof(VAL_EVENT_T));
        if (VAL_RESULT_NO_ERROR != eValHWLockRet)
        {
            MFV_LOGE("[VCODEC][ERROR] ISR set dec_isr error\n");
        }
        spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);
        return;
    }
    else
    {
        // Clear interrupt
        VDO_HW_WRITE(KVA_VDEC_MISC_BASE + 41 * 4, VDO_HW_READ(KVA_VDEC_MISC_BASE + 41 * 4) | 0x11);
        VDO_HW_WRITE(KVA_VDEC_MISC_BASE + 41 * 4, VDO_HW_READ(KVA_VDEC_MISC_BASE + 41 * 4) & ~0x10);
    }

    spin_lock_irqsave(&DecIsrLock, ulFlags);
    eValRet = eVideoSetEvent(&DecIsrEvent, sizeof(VAL_EVENT_T));
    if (VAL_RESULT_NO_ERROR != eValRet)
    {
        MFV_LOGE("[VCODEC][ERROR] ISR set DecIsrEvent error\n");
    }
    spin_unlock_irqrestore(&DecIsrLock, ulFlags);

    return;
}


void enc_isr(void)
{
    VAL_RESULT_T  eValRet;
    VAL_UINT32_T index, i, maxnum;
    VAL_ULONG_T ulFlags, ulFlagsISR, ulFlagsLockHW, ulFlagsHWLock;
    VAL_UINT32_T u4IRQStatus = 0;

    VAL_UINT32_T u4TempEncISRCount = 0;
    VAL_UINT32_T u4TempLockEncHWCount = 0;
    VAL_RESULT_T  eValHWLockRet = VAL_RESULT_INVALID_ISR;
    //----------------------

    spin_lock_irqsave(&EncISRCountLock, ulFlagsISR);
    gu4EncISRCount++;
    u4TempEncISRCount = gu4EncISRCount;
    spin_unlock_irqrestore(&EncISRCountLock, ulFlagsISR);

    spin_lock_irqsave(&LockEncHWCountLock, ulFlagsLockHW);
    u4TempLockEncHWCount = gu4LockEncHWCount;
    spin_unlock_irqrestore(&LockEncHWCountLock, ulFlagsLockHW);

    if (u4TempEncISRCount != u4TempLockEncHWCount)
    {
        //MFV_LOGE("[INFO] Enc ISRCount: 0x%x, LockHWCount:0x%x\n", u4TempEncISRCount, u4TempLockEncHWCount);
    }

    if (grVcodecHWLock.pvHandle == 0)
    {
        MFV_LOGE("[VCODEC][ERROR][ISR] NO one Lock Enc HW, please check!!\n");

        // Clear all status
        if (grVcodecHWLock.eDriverType == VAL_DRIVER_TYPE_H264_ENC)
        {
            VDO_HW_WRITE(KVA_VENC_IRQ_ACK_ADDR, VENC_IRQ_STATUS_PAUSE);
            //VDO_HW_WRITE(KVA_VENC_IRQ_ACK_ADDR, VENC_IRQ_STATUS_DRAM_VP8);
            VDO_HW_WRITE(KVA_VENC_IRQ_ACK_ADDR, VENC_IRQ_STATUS_SWITCH);
            VDO_HW_WRITE(KVA_VENC_IRQ_ACK_ADDR, VENC_IRQ_STATUS_DRAM);
            VDO_HW_WRITE(KVA_VENC_IRQ_ACK_ADDR, VENC_IRQ_STATUS_SPS);
            VDO_HW_WRITE(KVA_VENC_IRQ_ACK_ADDR, VENC_IRQ_STATUS_PPS);
            VDO_HW_WRITE(KVA_VENC_IRQ_ACK_ADDR, VENC_IRQ_STATUS_FRM);
        }
        else if (grVcodecHWLock.eDriverType == VAL_DRIVER_TYPE_MP4_ENC)
        {
            VDO_HW_WRITE(KVA_VENC_IRQ_ACK_ADDR, 1);
        }

        return;
    }

    if (grVcodecHWLock.eDriverType == VAL_DRIVER_TYPE_H264_ENC) // hardwire
    {
        gu4HwVencIrqStatus = VDO_HW_READ(KVA_VENC_IRQ_STATUS_ADDR);
        if (gu4HwVencIrqStatus & VENC_IRQ_STATUS_PAUSE)
        {
            VDO_HW_WRITE(KVA_VENC_IRQ_ACK_ADDR, VENC_IRQ_STATUS_PAUSE);
        }
        if (gu4HwVencIrqStatus & VENC_IRQ_STATUS_SWITCH)
        {
            VDO_HW_WRITE(KVA_VENC_IRQ_ACK_ADDR, VENC_IRQ_STATUS_SWITCH);
        }
        if (gu4HwVencIrqStatus & VENC_IRQ_STATUS_DRAM)
        {
            VDO_HW_WRITE(KVA_VENC_IRQ_ACK_ADDR, VENC_IRQ_STATUS_DRAM);
        }
        if (gu4HwVencIrqStatus & VENC_IRQ_STATUS_SPS)
        {
            VDO_HW_WRITE(KVA_VENC_IRQ_ACK_ADDR, VENC_IRQ_STATUS_SPS);
        }
        if (gu4HwVencIrqStatus & VENC_IRQ_STATUS_PPS)
        {
            VDO_HW_WRITE(KVA_VENC_IRQ_ACK_ADDR, VENC_IRQ_STATUS_PPS);
        }
        if (gu4HwVencIrqStatus & VENC_IRQ_STATUS_FRM)
        {
            VDO_HW_WRITE(KVA_VENC_IRQ_ACK_ADDR, VENC_IRQ_STATUS_FRM);
        }
    }
    else if (grVcodecHWLock.eDriverType == VAL_DRIVER_TYPE_HEVC_ENC) // hardwire
    {
        MFV_LOGE("[VCODEC][ISR] VAL_DRIVER_TYPE_HEVC_ENC!!\n");
    }
    else if (grVcodecHWLock.eDriverType == VAL_DRIVER_TYPE_MP4_ENC)
    {
        spin_lock_irqsave(&OalHWContextLock, ulFlags);
        index = search_HWLockSlot_ByHandle(0, (VAL_HANDLE_T)grVcodecHWLock.pvHandle);

        //MFV_LOGD("index = %d\n", index);

        // in case, if the process is killed first,
        // then receive an ISR from HW, the event information already cleared.
        if (index == -1) // Hybrid
        {
            MFV_LOGE("[VCODEC][ERROR][ISR] Can't find any index in ISR\n");

            VDO_HW_WRITE(KVA_VENC_IRQ_ACK_ADDR, 1);
            spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

            return;
        }

        // get address from context
        //MFV_LOGD("ISR: Total %d u4NumOfRegister\n", oal_hw_context[index].u4NumOfRegister);

        maxnum = oal_hw_context[index].u4NumOfRegister;
        if (oal_hw_context[index].u4NumOfRegister > VCODEC_MULTIPLE_INSTANCE_NUM)
        {
            MFV_LOGE("[VCODEC][ERROR][ISR] oal_hw_context[index].u4NumOfRegister =%d\n",
                    oal_hw_context[index].u4NumOfRegister);
            maxnum = VCODEC_MULTIPLE_INSTANCE_NUM;
        }

        //MFV_LOGD("oal_hw_context[index].kva_u4HWIsCompleted 0x%x value=%d \n",
        //oal_hw_context[index].kva_u4HWIsCompleted,
        //*((volatile VAL_UINT32_T*)oal_hw_context[index].kva_u4HWIsCompleted));
        if ((((volatile VAL_UINT32_T *)oal_hw_context[index].kva_u4HWIsCompleted) == NULL) ||
                (((volatile VAL_UINT32_T *)oal_hw_context[index].kva_u4HWIsTimeout) == NULL))
        {
            MFV_LOGE("[VCODEC][ERROR][ISR] index = %d, please check!!\n", index);

            VDO_HW_WRITE(KVA_VENC_IRQ_ACK_ADDR, 1);
            spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

            return;
        }
        *((volatile VAL_UINT32_T *)oal_hw_context[index].kva_u4HWIsCompleted) = 1;
        *((volatile VAL_UINT32_T *)oal_hw_context[index].kva_u4HWIsTimeout) = 0;

        for (i = 0; i < maxnum ; i++)
        {
            //MFV_LOGD("[BEFORE] ISR read: [%d]  User_va=0x%x kva=0x%x 0x%x \n", i ,
            //*((volatile VAL_UINT32_T*)oal_hw_context[index].kva_Oal_HW_mem_reg + i*2),
            //oal_hw_context[index].oalmem_status[i].u4ReadAddr,
            //*((volatile VAL_UINT32_T*)oal_hw_context[index].kva_Oal_HW_mem_reg + i*2 + 1));

            *((volatile VAL_UINT32_T *)oal_hw_context[index].kva_Oal_HW_mem_reg + i * 2 + 1) = 
                *((volatile VAL_UINT32_T *)oal_hw_context[index].oalmem_status[i].u4ReadAddr);

            if (maxnum == 3)
            {
                if (i == 0)
                {
                    u4IRQStatus = (*((volatile VAL_UINT32_T *)oal_hw_context[index].kva_Oal_HW_mem_reg + i * 2 + 1));
                    if (u4IRQStatus != 2)
                    {
                        MFV_LOGE("[VCODEC][ERROR][ISR] IRQ status error u4IRQStatus = %d\n", u4IRQStatus);
                    }
                }

                if (u4IRQStatus != 2)
                {
                    MFV_LOGE("[VCODEC][ERROR][ISR] %d, 0x%lx, %d, %d, %d, %d\n",
                             i,
                             (VAL_ULONG_T)((volatile VAL_UINT32_T *)oal_hw_context[index].oalmem_status[i].u4ReadAddr),
                             (*((volatile VAL_UINT32_T *)oal_hw_context[index].kva_Oal_HW_mem_reg + i * 2 + 1)),
                             VDO_HW_READ(KVA_VENC_IRQ_ACK_ADDR),
                             VDO_HW_READ(KVA_VENC_ZERO_COEF_COUNT_ADDR),
                             VDO_HW_READ(KVA_VENC_BYTE_COUNT_ADDR));
                }
            }

            //MFV_LOGD("[AFTER] ISR read: [%d]  User_va=0x%x kva=0x%x 0x%x \n", i ,
            //*((volatile VAL_UINT32_T*)oal_hw_context[index].kva_Oal_HW_mem_reg + i*2),
            //oal_hw_context[index].oalmem_status[i].u4ReadAddr,
            //*((volatile VAL_UINT32_T*)oal_hw_context[index].kva_Oal_HW_mem_reg + i*2 + 1) /*oal_hw_context[index].oalmem_status[i].u4ReadData*/);
        }
        //set handle event
        eValRet = eVideoSetEvent(&oal_hw_context[index].IsrEvent, sizeof(VAL_EVENT_T));
        spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

        // Clear interrupt
        // ACK interrupt
        // encoder
        VDO_HW_WRITE(KVA_VENC_IRQ_ACK_ADDR, 1);

        //Unlock
        spin_lock_irqsave(&HWLock, ulFlagsHWLock);
        for (i = 0; i < VCODEC_THREAD_MAX_NUM; i++)
        {
            grVcodecHWLock.u4VCodecThreadID[i] = -1;
        }
        grVcodecHWLock.u4VCodecThreadNum = VCODEC_THREAD_MAX_NUM;
        grVcodecHWLock.rLockedTime.u4Sec = 0;
        grVcodecHWLock.rLockedTime.u4uSec = 0;
        grVcodecHWLock.pvHandle = 0;
        disable_irq_nosync(VENC_IRQ_ID);
        --gu4VEncIRQCount;
        // turn venc power off
        venc_power_off();

        eValHWLockRet = eVideoSetEvent(&HWLockEvent, sizeof(VAL_EVENT_T));
        if (VAL_RESULT_NO_ERROR != eValHWLockRet)
        {
            MFV_LOGE("[VCODEC][ERROR] ISR set enc_isr error\n");
        }
        spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);

        return;
    }
    else
    {
        MFV_LOGE("[VCODEC][ERROR] Invalid lock holder driver type = %d\n", grVcodecHWLock.eDriverType);
    }

    spin_lock_irqsave(&EncIsrLock, ulFlags);
    eValRet = eVideoSetEvent(&EncIsrEvent, sizeof(VAL_EVENT_T));
    if (VAL_RESULT_NO_ERROR != eValRet)
    {
        MFV_LOGE("[VCODEC][ERROR] ISR set EncIsrEvent error\n");
    }
    spin_unlock_irqrestore(&EncIsrLock, ulFlags);
}

static irqreturn_t video_intr_dlr(int irq, void *dev_id)
{
    dec_isr();
    return IRQ_HANDLED;
}

static irqreturn_t video_intr_dlr2(int irq, void *dev_id)
{
    enc_isr();
    return IRQ_HANDLED;
}

static long vcodec_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    VAL_LONG_T ret;
    VAL_UINT8_T *user_data_addr;
    VAL_RESULT_T  eValRet;
    VAL_RESULT_T  eValHWLockRet = VAL_RESULT_INVALID_ISR;
    VAL_ULONG_T ulFlags, ulFlagsLockHW, ulFlagsISR, ulFlagsHWLock;
    VAL_HW_LOCK_T rHWLock;
    VAL_BOOL_T  bLockedHW = VAL_FALSE;
    VAL_UINT32_T FirstUseDecHW = 0;
    VAL_UINT32_T FirstUseEncHW = 0;
    VAL_TIME_T rCurTime;
    VAL_UINT32_T u4TimeInterval;
    VAL_ISR_T  val_isr;
    VAL_VCODEC_CORE_LOADING_T rTempCoreLoading;
    VAL_VCODEC_CPU_OPP_LIMIT_T rCpuOppLimit;
    VAL_INT32_T temp_nr_cpu_ids;
    VAL_POWER_T rPowerParam;
    VAL_MEMORY_T rTempMem;
    VAL_VCODEC_THREAD_ID_T rTempTID;
    VAL_UINT32_T u4Index = 0xff;
    VAL_UINT32_T *pu4TempKVA;
    VAL_ULONG_T  u8TempKPA;
    VAL_UINT32_T u4TempVCodecThreadNum;
    VAL_UINT32_T u4TempVCodecThreadID[VCODEC_THREAD_MAX_NUM];
    unsigned long   srhRslt = 0;
    int i, j;

#ifdef ENABLE_MMDVFS_VDEC
    VAL_UINT32_T _monitor_duration = 0;
    VAL_UINT32_T _diff = 0;
    VAL_UINT32_T _perc = 0;
#endif

    switch (cmd)
    {
        case VCODEC_SET_THREAD_ID:
        {
            MFV_LOGD("VCODEC_SET_THREAD_ID + tid = %d\n", current->pid);
            user_data_addr = (VAL_UINT8_T *)arg;
            ret = copy_from_user(&rTempTID, user_data_addr, sizeof(VAL_VCODEC_THREAD_ID_T));
            if (ret)
            {
                MFV_LOGE("[ERROR] VCODEC_SET_THREAD_ID, copy_from_user failed: %ld\n", ret);
                return -EFAULT;
            }

            spin_lock_irqsave(&OalHWContextLock, ulFlags);
            setCurr_HWLockSlot_Thread_ID(rTempTID, &u4Index);
            spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

            if (u4Index == 0xff)
            {
                MFV_LOGE("[ERROR] VCODEC_SET_THREAD_ID error, u4Index = %d\n", u4Index);
            }

            MFV_LOGD("VCODEC_SET_THREAD_ID - tid = %d\n", current->pid);
        }
        break;
        case VCODEC_ALLOC_NON_CACHE_BUFFER:
        {
            VAL_INT32_T i4I;
            VAL_INT32_T i4Index = -1;
            MFV_LOGE("VCODEC_ALLOC_NON_CACHE_BUFFER + tid = %d\n", current->pid);

            user_data_addr = (VAL_UINT8_T *)arg;
            ret = copy_from_user(&rTempMem, user_data_addr, sizeof(VAL_MEMORY_T));
            if (ret)
            {
                MFV_LOGE("[ERROR] VCODEC_ALLOC_NON_CACHE_BUFFER, copy_from_user failed: %lu\n", ret);
                return -EFAULT;
            }

            rTempMem.u4ReservedSize /*kernel va*/ = (VAL_ULONG_T)dma_alloc_coherent(vcodec_device, rTempMem.u4MemSize, (dma_addr_t *)&rTempMem.pvMemPa, GFP_KERNEL);
            if ((0 == rTempMem.u4ReservedSize) || (0 == rTempMem.pvMemPa))
            {
                MFV_LOGE("[ERROR] dma_alloc_coherent fail in VCODEC_ALLOC_NON_CACHE_BUFFER, size=%lu\n", rTempMem.u4MemSize);
                return -EFAULT;
            }

            MFV_LOGD("[VCODEC] kernel va = 0x%lx, kernel pa = 0x%lx, memory size = %lu\n",
                     (VAL_ULONG_T)rTempMem.u4ReservedSize, (VAL_ULONG_T)rTempMem.pvMemPa, (VAL_ULONG_T)rTempMem.u4MemSize);

            spin_lock_irqsave(&OalHWContextLock, ulFlags);
            i4Index = search_HWLockSlot_ByTID(0, current->pid);
            if (i4Index == -1)
            {
                MFV_LOGE("[VCODEC][ERROR] Add_NonCacheMemoryList error, u4Index = -1\n");
                spin_unlock_irqrestore(&OalHWContextLock, ulFlags);
                break;
            }

            u4TempVCodecThreadNum = oal_hw_context[i4Index].u4VCodecThreadNum;
            for (i4I = 0; i4I < u4TempVCodecThreadNum; i4I++)
            {
                u4TempVCodecThreadID[i4I] = oal_hw_context[i4Index].u4VCodecThreadID[i4I];
            }
            spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

            mutex_lock(&NonCacheMemoryListLock);
            Add_NonCacheMemoryList(rTempMem.u4ReservedSize, (VAL_ULONG_T)rTempMem.pvMemPa, (VAL_ULONG_T)rTempMem.u4MemSize, u4TempVCodecThreadNum, u4TempVCodecThreadID);
            mutex_unlock(&NonCacheMemoryListLock);

            ret = copy_to_user(user_data_addr, &rTempMem, sizeof(VAL_MEMORY_T));
            if (ret)
            {
                MFV_LOGE("[ERROR] VCODEC_ALLOC_NON_CACHE_BUFFER, copy_to_user failed: %lu\n", ret);
                return -EFAULT;
            }

            MFV_LOGE("VCODEC_ALLOC_NON_CACHE_BUFFER - tid = %d\n", current->pid);
        }
        break;

        case VCODEC_FREE_NON_CACHE_BUFFER:
        {
            MFV_LOGE("VCODEC_FREE_NON_CACHE_BUFFER + tid = %d\n", current->pid);

            user_data_addr = (VAL_UINT8_T *)arg;
            ret = copy_from_user(&rTempMem, user_data_addr, sizeof(VAL_MEMORY_T));
            if (ret)
            {
                MFV_LOGE("[ERROR] VCODEC_FREE_NON_CACHE_BUFFER, copy_from_user failed: %lu\n", ret);
                return -EFAULT;
            }

            dma_free_coherent(vcodec_device, rTempMem.u4MemSize, (void *)rTempMem.u4ReservedSize, (dma_addr_t)rTempMem.pvMemPa);

            mutex_lock(&NonCacheMemoryListLock);
            Free_NonCacheMemoryList((VAL_ULONG_T)rTempMem.u4ReservedSize, (VAL_ULONG_T)rTempMem.pvMemPa);
            mutex_unlock(&NonCacheMemoryListLock);

            rTempMem.u4ReservedSize = 0;
            rTempMem.pvMemPa = NULL;

            ret = copy_to_user(user_data_addr, &rTempMem, sizeof(VAL_MEMORY_T));
            if (ret)
            {
                MFV_LOGE("[ERROR] VCODEC_FREE_NON_CACHE_BUFFER, copy_to_user failed: %lu\n", ret);
                return -EFAULT;
            }

            MFV_LOGE("VCODEC_FREE_NON_CACHE_BUFFER - tid = %d\n", current->pid);
        }
        break;


        case VCODEC_INC_DEC_EMI_USER:
        {
            MFV_LOGD("VCODEC_INC_DEC_EMI_USER + tid = %d\n", current->pid);

            mutex_lock(&DecEMILock);
            gu4DecEMICounter++;
            MFV_LOGE("[VCODEC] DEC_EMI_USER = %d\n", gu4DecEMICounter);
            user_data_addr = (VAL_UINT8_T *)arg;
            ret = copy_to_user(user_data_addr, &gu4DecEMICounter, sizeof(VAL_UINT32_T));
            if (ret)
            {
                MFV_LOGE("[ERROR] VCODEC_INC_DEC_EMI_USER, copy_to_user failed: %lu\n", ret);
                mutex_unlock(&DecEMILock);
                return -EFAULT;
            }
            mutex_unlock(&DecEMILock);

#ifdef ENABLE_MMDVFS_VDEC
            // MM DVFS related
            MFV_LOGE("[VCODEC][MMDVFS_VDEC] @@ INC_DEC_EMI MM DVFS init");
            // raise voltage
            SendDvfsRequest(DVFS_DEFAULT);
            VdecDvfsBegin();
#endif

            MFV_LOGD("VCODEC_INC_DEC_EMI_USER - tid = %d\n", current->pid);
        }
        break;

        case VCODEC_DEC_DEC_EMI_USER:
        {
            MFV_LOGD("VCODEC_DEC_DEC_EMI_USER + tid = %d\n", current->pid);

            mutex_lock(&DecEMILock);
            gu4DecEMICounter--;
            MFV_LOGE("[VCODEC] DEC_EMI_USER = %d\n", gu4DecEMICounter);
            user_data_addr = (VAL_UINT8_T *)arg;
            ret = copy_to_user(user_data_addr, &gu4DecEMICounter, sizeof(VAL_UINT32_T));
            if (ret)
            {
                MFV_LOGE("[ERROR] VCODEC_DEC_DEC_EMI_USER, copy_to_user failed: %lu\n", ret);
                mutex_unlock(&DecEMILock);
                return -EFAULT;
            }
            mutex_unlock(&DecEMILock);

            MFV_LOGD("VCODEC_DEC_DEC_EMI_USER - tid = %d\n", current->pid);
        }
        break;

        case VCODEC_INC_ENC_EMI_USER:
        {
            MFV_LOGD("VCODEC_INC_ENC_EMI_USER + tid = %d\n", current->pid);

            mutex_lock(&EncEMILock);
            gu4EncEMICounter++;
            MFV_LOGE("[VCODEC] ENC_EMI_USER = %d\n", gu4EncEMICounter);
            user_data_addr = (VAL_UINT8_T *)arg;
            ret = copy_to_user(user_data_addr, &gu4EncEMICounter, sizeof(VAL_UINT32_T));
            if (ret)
            {
                MFV_LOGE("[ERROR] VCODEC_INC_ENC_EMI_USER, copy_to_user failed: %lu\n", ret);
                mutex_unlock(&EncEMILock);
                return -EFAULT;
            }
            mutex_unlock(&EncEMILock);

            MFV_LOGD("VCODEC_INC_ENC_EMI_USER - tid = %d\n", current->pid);
        }
        break;

        case VCODEC_DEC_ENC_EMI_USER:
        {
            MFV_LOGD("VCODEC_DEC_ENC_EMI_USER + tid = %d\n", current->pid);

            mutex_lock(&EncEMILock);
            gu4EncEMICounter--;
            MFV_LOGE("[VCODEC] ENC_EMI_USER = %d\n", gu4EncEMICounter);
            user_data_addr = (VAL_UINT8_T *)arg;
            ret = copy_to_user(user_data_addr, &gu4EncEMICounter, sizeof(VAL_UINT32_T));
            if (ret)
            {
                MFV_LOGE("[ERROR] VCODEC_DEC_ENC_EMI_USER, copy_to_user failed: %lu\n", ret);
                mutex_unlock(&EncEMILock);
                return -EFAULT;
            }
            mutex_unlock(&EncEMILock);

            MFV_LOGD("VCODEC_DEC_ENC_EMI_USER - tid = %d\n", current->pid);
        }
        break;

        case VCODEC_LOCKHW:
        {
            MFV_LOGD("VCODEC_LOCKHW + tid = %d\n", current->pid);
            user_data_addr = (VAL_UINT8_T *)arg;
            ret = copy_from_user(&rHWLock, user_data_addr, sizeof(VAL_HW_LOCK_T));
            if (ret)
            {
                MFV_LOGE("[ERROR] VCODEC_LOCKHW, copy_from_user failed: %lu\n", ret);
                return -EFAULT;
            }

            MFV_LOGD("[VCODEC] LOCKHW eDriverType = %d\n", rHWLock.eDriverType);
            eValRet = VAL_RESULT_INVALID_ISR;
            if (rHWLock.eDriverType == VAL_DRIVER_TYPE_MP4_DEC ||
                rHWLock.eDriverType == VAL_DRIVER_TYPE_HEVC_DEC ||
                rHWLock.eDriverType == VAL_DRIVER_TYPE_H264_DEC ||
                rHWLock.eDriverType == VAL_DRIVER_TYPE_MP1_MP2_DEC ||
                rHWLock.eDriverType == VAL_DRIVER_TYPE_VC1_DEC ||
                rHWLock.eDriverType == VAL_DRIVER_TYPE_VC1_ADV_DEC ||
                rHWLock.eDriverType == VAL_DRIVER_TYPE_VP8_DEC)
            {
                while (bLockedHW == VAL_FALSE)
                {
                    mutex_lock(&HWLockEventTimeoutLock);
                    if (HWLockEvent.u4TimeoutMs == 1)
                    {
                        MFV_LOGE("VCODEC_LOCKHW, First Use Dec HW!!\n");
                        FirstUseDecHW = 1;
                    }
                    else
                    {
                        FirstUseDecHW = 0;
                    }
                    mutex_unlock(&HWLockEventTimeoutLock);

                    if (FirstUseDecHW == 1)
                    {
                        eValRet = eVideoWaitEvent(&HWLockEvent, sizeof(VAL_EVENT_T));
                    }
                    mutex_lock(&HWLockEventTimeoutLock);
                    if (HWLockEvent.u4TimeoutMs != 1000)
                    {
                        HWLockEvent.u4TimeoutMs = 1000;
                        FirstUseDecHW = 1;
                    }
                    else
                    {
                        FirstUseDecHW = 0;
                    }
                    mutex_unlock(&HWLockEventTimeoutLock);

                    spin_lock_irqsave(&HWLock, ulFlagsHWLock);
                    // one process try to lock twice
                    if (grVcodecHWLock.pvHandle == (VAL_VOID_T *)pmem_user_v2p_video((VAL_ULONG_T)rHWLock.pvHandle))
                    {
                        MFV_LOGE("[WARNING] VCODEC_LOCKHW, one decoder instance try to lock twice, may cause lock HW timeout!! instance = 0x%lx, CurrentTID = %d\n",
                                 (VAL_ULONG_T)grVcodecHWLock.pvHandle, current->pid);
                    }
                    spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);

                    if (FirstUseDecHW == 0)
                    {
                        MFV_LOGD("VCODEC_LOCKHW, Not first time use HW, timeout = %d\n", HWLockEvent.u4TimeoutMs);
                        eValRet = eVideoWaitEvent(&HWLockEvent, sizeof(VAL_EVENT_T));
                    }

                    if (VAL_RESULT_INVALID_ISR == eValRet)
                    {
                        MFV_LOGE("[ERROR] VCODEC_LOCKHW, HWLockEvent TimeOut, CurrentTID = %d\n", current->pid);
                        if (FirstUseDecHW != 1)
                        {
                            spin_lock_irqsave(&HWLock, ulFlagsHWLock);
                            if (grVcodecHWLock.pvHandle == 0)
                            {
                                MFV_LOGE("[WARNING] VCODEC_LOCKHW, maybe mediaserver restart before, please check!!\n");
                            }
                            else
                            {
                                MFV_LOGE("[WARNING] VCODEC_LOCKHW, someone use HW, and check timeout value!!\n");
                            }
                            spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);
                        }
                    }
                    else if (VAL_RESULT_RESTARTSYS == eValRet)
                    {
                        MFV_LOGE("[WARNING] VCODEC_LOCKHW, VAL_RESULT_RESTARTSYS return when HWLock!!\n");
                        return -ERESTARTSYS;
                    }

                    spin_lock_irqsave(&HWLock, ulFlagsHWLock);
                    if (grVcodecHWLock.pvHandle == 0) // No one holds dec hw lock now
                    {
                        if (rHWLock.eDriverType == VAL_DRIVER_TYPE_H264_DEC || rHWLock.eDriverType == VAL_DRIVER_TYPE_VP8_DEC)    //for H264 hybrid decoder
                        {
                            spin_lock_irqsave(&OalHWContextLock, ulFlags);
                            u4Index = search_HWLockSlot_ByTID(0, current->pid);
                            //Index = search_HWLockSlot_ByHandle(0, pmem_user_v2p_video((unsigned int)rHWLock.pvHandle));
                            spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

                            if (u4Index == -1)
                            {
                                MFV_LOGE("[ERROR] VCODEC_LOCKHW, No process use enc HW, so current process can use HW, u4Index = -1\n");
                                spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);
                                return -EFAULT;
                            }

                            gu4VdecLockThreadId = current->pid;
                            grVcodecHWLock.pvHandle = (VAL_VOID_T *)pmem_user_v2p_video((VAL_ULONG_T)rHWLock.pvHandle);
                            grVcodecHWLock.u4ThreadID = current->pid;
                            MFV_LOGD("VCODEC_LOCKHW, No process use enc HW, so current process can use HW, handle = 0x%lx\n", (VAL_ULONG_T)grVcodecHWLock.pvHandle);

                            spin_lock_irqsave(&OalHWContextLock, ulFlags);
                            oal_hw_context[u4Index].pvHandle = (VAL_HANDLE_T)grVcodecHWLock.pvHandle;
                            //WaitISR need thread ID info.
                            grVcodecHWLock.u4VCodecThreadNum = oal_hw_context[u4Index].u4VCodecThreadNum;
                            for (i = 0; i < grVcodecHWLock.u4VCodecThreadNum; i++)
                            {
                                grVcodecHWLock.u4VCodecThreadID[i] = oal_hw_context[u4Index].u4VCodecThreadID[i];
                            }
                            spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

                            grVcodecHWLock.eDriverType = rHWLock.eDriverType;
                            eVideoGetTimeOfDay(&grVcodecHWLock.rLockedTime, sizeof(VAL_TIME_T));

                            MFV_LOGD("VCODEC_LOCKHW, LockInstance = 0x%lx CurrentTID = %d, rLockedTime(s, us) = %d, %d\n",
                                    (VAL_ULONG_T)grVcodecHWLock.pvHandle, current->pid, grVcodecHWLock.rLockedTime.u4Sec, grVcodecHWLock.rLockedTime.u4uSec);

                            bLockedHW = VAL_TRUE;
                            if (VAL_RESULT_INVALID_ISR == eValRet && FirstUseDecHW != 1)
                            {
                                vdec_power_off();
                                //disable_irq(VDEC_IRQ_ID);
                                //--gu4VDecIRQCount;
                                MFV_LOGE("[WARNING] VCODEC_LOCKHW, reset power when HWLock (%d)!!\n", gu4VDecIRQCount);
                            }
                            vdec_power_on();
                            //enable_irq(MT_VDEC_IRQ_ID);
                            enable_irq(VDEC_IRQ_ID);
                            ++gu4VDecIRQCount;
                        }
                        else    //other codecs
                        {
                            gu4VdecLockThreadId = current->pid;
                            grVcodecHWLock.pvHandle = (VAL_VOID_T *)pmem_user_v2p_video((VAL_ULONG_T)rHWLock.pvHandle);
                            grVcodecHWLock.eDriverType = rHWLock.eDriverType;
                            grVcodecHWLock.u4ThreadID = current->pid;
                            eVideoGetTimeOfDay(&grVcodecHWLock.rLockedTime, sizeof(VAL_TIME_T));

                            MFV_LOGD("VCODEC_LOCKHW, No process use dec HW, so current process can use HW\n");
                            MFV_LOGD("VCODEC_LOCKHW, LockInstance = 0x%lx CurrentTID = %d, rLockedTime(s, us) = %d, %d\n",
                                    (VAL_ULONG_T)grVcodecHWLock.pvHandle, current->pid, grVcodecHWLock.rLockedTime.u4Sec, grVcodecHWLock.rLockedTime.u4uSec);

                            bLockedHW = VAL_TRUE;
                            if (VAL_RESULT_INVALID_ISR == eValRet && FirstUseDecHW != 1)
                            {
                                MFV_LOGE("[WARNING] VCODEC_LOCKHW, reset power when HWLock!!\n");
                                vdec_power_off();
                                //disable_irq(VDEC_IRQ_ID);
                                //--gu4VDecIRQCount;
                            }
                            vdec_power_on();
                            //enable_irq(MT_VDEC_IRQ_ID);
                            enable_irq(VDEC_IRQ_ID);
                            ++gu4VDecIRQCount;

#ifdef ENABLE_MMDVFS_VDEC
                            // MM DVFS related
                            if (VAL_FALSE == gMMDFVFSMonitorStarts)
                            {
                                // Continous monitoring
                                VdecDvfsBegin();
                            }
                            if (VAL_TRUE == gMMDFVFSMonitorStarts)
                            {
                                MFV_LOGD("[VCODEC][MMDVFS_VDEC] @@ LOCK 1");
                                if (gMMDFVFSMonitorCounts > MONITOR_START_MINUS_1)
                                {
                                    if (VAL_TRUE == gFirstDvfsLock)
                                    {
                                        gFirstDvfsLock = VAL_FALSE;
                                        MFV_LOGE("[VCODEC][MMDVFS_VDEC] @@ LOCK 1 start monitor");
                                        eVideoGetTimeOfDay(&gMMDFVFSMonitorStartTime, sizeof(VAL_TIME_T));
                                    }
                                    eVideoGetTimeOfDay(&gMMDFVFSLastLockTime, sizeof(VAL_TIME_T));
                                }
                            }
#endif
                        }

                    }
                    else // Another one holding dec hw now
                    {
                        MFV_LOGE("VCODEC_LOCKHW E\n");
                        eVideoGetTimeOfDay(&rCurTime, sizeof(VAL_TIME_T));
                        u4TimeInterval = (((((rCurTime.u4Sec - grVcodecHWLock.rLockedTime.u4Sec) * 1000000) + rCurTime.u4uSec)
                                           - grVcodecHWLock.rLockedTime.u4uSec) / 1000);

                        MFV_LOGD("VCODEC_LOCKHW, someone use dec HW, and check timeout value\n");
                        MFV_LOGD("VCODEC_LOCKHW, Instance = 0x%lx CurrentTID = %d, TimeInterval(ms) = %d, TimeOutValue(ms)) = %d\n",
                                 (VAL_ULONG_T)grVcodecHWLock.pvHandle, current->pid, u4TimeInterval, rHWLock.u4TimeoutMs);

                        MFV_LOGE("VCODEC_LOCKHW Lock Instance = 0x%lx, Lock TID = %d, CurrentTID = %d, rLockedTime(%d s, %d us), rCurTime(%d s, %d us)\n",
                                 (VAL_ULONG_T)grVcodecHWLock.pvHandle, gu4VdecLockThreadId, current->pid,
                                 grVcodecHWLock.rLockedTime.u4Sec, grVcodecHWLock.rLockedTime.u4uSec,
                                 rCurTime.u4Sec, rCurTime.u4uSec
                                );

                        // 2012/12/16. Cheng-Jung Never steal hardware lock
                        if (0)
                            //if (u4TimeInterval >= rHWLock.u4TimeoutMs)
                        {
                            grVcodecHWLock.pvHandle = (VAL_VOID_T *)pmem_user_v2p_video((VAL_ULONG_T)rHWLock.pvHandle);
                            grVcodecHWLock.u4ThreadID = current->pid;
                            grVcodecHWLock.eDriverType = rHWLock.eDriverType;
                            eVideoGetTimeOfDay(&grVcodecHWLock.rLockedTime, sizeof(VAL_TIME_T));
                            bLockedHW = VAL_TRUE;
                            vdec_power_on();
                            // TODO: Error handling, VDEC break, reset?
                        }
                    }
                    spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);
                    spin_lock_irqsave(&LockDecHWCountLock, ulFlagsLockHW);
                    gu4LockDecHWCount++;
                    spin_unlock_irqrestore(&LockDecHWCountLock, ulFlagsLockHW);
                }
            }
            else if (rHWLock.eDriverType == VAL_DRIVER_TYPE_H264_ENC ||
                     rHWLock.eDriverType == VAL_DRIVER_TYPE_HEVC_ENC ||
                     rHWLock.eDriverType == VAL_DRIVER_TYPE_JPEG_ENC ||
                     rHWLock.eDriverType == VAL_DRIVER_TYPE_MP4_ENC)
            {
                while (bLockedHW == VAL_FALSE)
                {
                    // Early break for JPEG VENC
                    if (rHWLock.u4TimeoutMs == 0)
                    {
                        if (grVcodecHWLock.pvHandle != 0)
                        {
                            break;
                        }
                    }

                    // Wait to acquire Enc HW lock
                    mutex_lock(&HWLockEventTimeoutLock);
                    if (HWLockEvent.u4TimeoutMs == 1)
                    {
                        MFV_LOGE("VCODEC_LOCKHW, First Use Enc HW %d!!\n", rHWLock.eDriverType);
                        FirstUseEncHW = 1;
                    }
                    else
                    {
                        FirstUseEncHW = 0;
                    }
                    mutex_unlock(&HWLockEventTimeoutLock);
                    if (FirstUseEncHW == 1)
                    {
                        eValRet = eVideoWaitEvent(&HWLockEvent, sizeof(VAL_EVENT_T));
                    }

                    mutex_lock(&HWLockEventTimeoutLock);
                    if (HWLockEvent.u4TimeoutMs == 1)
                    {
                        HWLockEvent.u4TimeoutMs = 1000;
                        FirstUseEncHW = 1;
                    }
                    else
                    {
                        FirstUseEncHW = 0;
                        if (rHWLock.u4TimeoutMs == 0)
                        {
                            HWLockEvent.u4TimeoutMs = 0; // No wait
                        }
                        else
                        {
                            HWLockEvent.u4TimeoutMs = 1000; // Wait indefinitely
                        }
                    }
                    mutex_unlock(&HWLockEventTimeoutLock);

                    spin_lock_irqsave(&HWLock, ulFlagsHWLock);
                    // one process try to lock twice
                    if (grVcodecHWLock.pvHandle == (VAL_VOID_T *)pmem_user_v2p_video((VAL_ULONG_T)rHWLock.pvHandle))
                    {
                        MFV_LOGE("[WARNING] VCODEC_LOCKHW, one encoder instance try to lock twice, may cause lock HW timeout!! instance = 0x%lx, CurrentTID = %d, type:%d\n",
                                 (VAL_ULONG_T)grVcodecHWLock.pvHandle, current->pid, rHWLock.eDriverType);
                    }
                    spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);

                    if (FirstUseEncHW == 0)
                    {
                        eValRet = eVideoWaitEvent(&HWLockEvent, sizeof(VAL_EVENT_T));
                    }

                    if (VAL_RESULT_INVALID_ISR == eValRet)
                    {
                        MFV_LOGE("[ERROR] VCODEC_LOCKHW HWLockEvent TimeOut, CurrentTID = %d\n", current->pid);
                        if (FirstUseEncHW != 1)
                        {
                            spin_lock_irqsave(&HWLock, ulFlagsHWLock);
                            if (grVcodecHWLock.pvHandle == 0)
                            {
                                MFV_LOGE("[WARNING] VCODEC_LOCKHW, maybe mediaserver restart before, please check!!\n");
                            }
                            else
                            {
                                MFV_LOGE("[WARNING] VCODEC_LOCKHW, someone use HW, and check timeout value!! %d\n", gLockTimeOutCount);
                                ++gLockTimeOutCount;
                                if (gLockTimeOutCount > 30)
                                {
                                    MFV_LOGE("[ERROR] VCODEC_LOCKHW - ID %d  fail, someone locked HW time out more than 30 times 0x%lx, %lx, 0x%lx, type:%d\n", current->pid, (VAL_ULONG_T)grVcodecHWLock.pvHandle, pmem_user_v2p_video((VAL_ULONG_T)rHWLock.pvHandle), (VAL_ULONG_T)rHWLock.pvHandle, rHWLock.eDriverType);
                                    gLockTimeOutCount = 0;
                                    spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);
                                    return -EFAULT;
                                }

                                if (rHWLock.u4TimeoutMs == 0)
                                {
                                    MFV_LOGE("[ERROR] VCODEC_LOCKHW - ID %d  fail, someone locked HW already 0x%lx, %lx, 0x%lx, type:%d\n", current->pid, (VAL_ULONG_T)grVcodecHWLock.pvHandle, pmem_user_v2p_video((VAL_ULONG_T)rHWLock.pvHandle), (VAL_ULONG_T)rHWLock.pvHandle, rHWLock.eDriverType);
                                    gLockTimeOutCount = 0;
                                    spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);
                                    return -EFAULT;
                                }
                            }
                            spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);
                        }
                    }
                    else if (VAL_RESULT_RESTARTSYS == eValRet)
                    {
                        return -ERESTARTSYS;
                    }

                    spin_lock_irqsave(&HWLock, ulFlagsHWLock);
                    if (grVcodecHWLock.pvHandle == 0)   //No process use HW, so current process can use HW
                    {
                        if (rHWLock.eDriverType == VAL_DRIVER_TYPE_MP4_ENC)
                        {
                            spin_lock_irqsave(&OalHWContextLock, ulFlags);
                            u4Index = search_HWLockSlot_ByTID(0, current->pid);
                            //Index = search_HWLockSlot_ByHandle(0, pmem_user_v2p_video((unsigned int)rHWLock.pvHandle));
                            spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

                            if (u4Index == -1)
                            {
                                MFV_LOGE("[ERROR] VCODEC_LOCKHW, No process use enc HW, so current process can use HW, u4Index = -1\n");
                                spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);
                                return -EFAULT;
                            }

                            grVcodecHWLock.pvHandle = (VAL_VOID_T *)pmem_user_v2p_video((unsigned long)rHWLock.pvHandle);
                            grVcodecHWLock.u4ThreadID = current->pid;
                            MFV_LOGD("VCODEC_LOCKHW, No process use enc HW, so current process can use HW, handle = 0x%lx\n", (VAL_ULONG_T)grVcodecHWLock.pvHandle);
                            grVcodecHWLock.eDriverType = rHWLock.eDriverType;
                            spin_lock_irqsave(&OalHWContextLock, ulFlags);
                            //WaitISR need thread ID info.
                            grVcodecHWLock.u4VCodecThreadNum = oal_hw_context[u4Index].u4VCodecThreadNum;
                            for (i = 0; i < grVcodecHWLock.u4VCodecThreadNum; i++)
                            {
                                grVcodecHWLock.u4VCodecThreadID[i] = oal_hw_context[u4Index].u4VCodecThreadID[i];
                            }
                            oal_hw_context[u4Index].pvHandle = (VAL_HANDLE_T)grVcodecHWLock.pvHandle;
                            spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

                            eVideoGetTimeOfDay(&grVcodecHWLock.rLockedTime, sizeof(VAL_TIME_T));

                            MFV_LOGD("VCODEC_LOCKHW, LockInstance = 0x%lx CurrentTID = %d, rLockedTime(s, us) = %d, %d\n",
                                     (VAL_ULONG_T)grVcodecHWLock.pvHandle, current->pid, grVcodecHWLock.rLockedTime.u4Sec, grVcodecHWLock.rLockedTime.u4uSec);

                            bLockedHW = VAL_TRUE;
                            venc_power_on();
                            enable_irq(VENC_IRQ_ID);
                            ++gu4VEncIRQCount;
                        }
                        else if (rHWLock.eDriverType == VAL_DRIVER_TYPE_H264_ENC ||
                                 rHWLock.eDriverType == VAL_DRIVER_TYPE_HEVC_ENC ||
                                 rHWLock.eDriverType == VAL_DRIVER_TYPE_JPEG_ENC)
                        {
                            grVcodecHWLock.pvHandle = (VAL_VOID_T *)pmem_user_v2p_video((VAL_ULONG_T)rHWLock.pvHandle);
                            MFV_LOGD("VCODEC_LOCKHW, No process use HW, so current process can use HW, handle = 0x%lx\n", (VAL_ULONG_T)grVcodecHWLock.pvHandle);
                            grVcodecHWLock.u4ThreadID = current->pid;
                            grVcodecHWLock.eDriverType = rHWLock.eDriverType;
                            eVideoGetTimeOfDay(&grVcodecHWLock.rLockedTime, sizeof(VAL_TIME_T));

                            MFV_LOGD("VCODEC_LOCKHW, No process use HW, so current process can use HW\n");
                            MFV_LOGD("VCODEC_LOCKHW, LockInstance = 0x%lx CurrentTID = %d, rLockedTime(s, us) = %d, %d\n",
                                     (VAL_ULONG_T)grVcodecHWLock.pvHandle, current->pid, grVcodecHWLock.rLockedTime.u4Sec, grVcodecHWLock.rLockedTime.u4uSec);

                            bLockedHW = VAL_TRUE;
                            if (rHWLock.eDriverType == VAL_DRIVER_TYPE_H264_ENC ||
                                rHWLock.eDriverType == VAL_DRIVER_TYPE_HEVC_ENC)
                            {
                                venc_power_on();
                                //enable_irq(MT_VENC_IRQ_ID);
                                enable_irq(VENC_IRQ_ID);
                                ++gu4VEncIRQCount;
                            }
                        }
                    }
                    else    //someone use HW, and check timeout value
                    {
                        if (rHWLock.u4TimeoutMs == 0)
                        {
                            bLockedHW = VAL_FALSE;
                            spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);
                            break;
                        }

                        eVideoGetTimeOfDay(&rCurTime, sizeof(VAL_TIME_T));
                        u4TimeInterval = (((((rCurTime.u4Sec - grVcodecHWLock.rLockedTime.u4Sec) * 1000000) + rCurTime.u4uSec)
                                           - grVcodecHWLock.rLockedTime.u4uSec) / 1000);

                        MFV_LOGD("VCODEC_LOCKHW, someone use enc HW, and check timeout value\n");
                        MFV_LOGD("VCODEC_LOCKHW, LockInstance = 0x%lx, CurrentInstance = 0x%lx, CurrentTID = %d, TimeInterval(ms) = %d, TimeOutValue(ms)) = %d\n",
                                 (VAL_ULONG_T)grVcodecHWLock.pvHandle, pmem_user_v2p_video((VAL_ULONG_T)rHWLock.pvHandle), current->pid, u4TimeInterval, rHWLock.u4TimeoutMs);

                        MFV_LOGD("VCODEC_LOCKHW, LockInstance = 0x%lx, CurrentInstance = 0x%lx, CurrentTID = %d, rLockedTime(s, us) = %d, %d, rCurTime(s, us) = %d, %d\n",
                                 (VAL_ULONG_T)grVcodecHWLock.pvHandle, pmem_user_v2p_video((VAL_ULONG_T)rHWLock.pvHandle), current->pid,
                                 grVcodecHWLock.rLockedTime.u4Sec, grVcodecHWLock.rLockedTime.u4uSec,
                                 rCurTime.u4Sec, rCurTime.u4uSec
                                );

                        ++gLockTimeOutCount;
                        if (gLockTimeOutCount > 30)
                        {
                            MFV_LOGE("[ERROR] VCODEC_LOCKHW - ID %d  fail, someone locked HW over 30 times without timeout 0x%lx, %lx, 0x%lx, type:%d\n", current->pid, (VAL_ULONG_T)grVcodecHWLock.pvHandle, pmem_user_v2p_video((VAL_ULONG_T)rHWLock.pvHandle), (VAL_ULONG_T)rHWLock.pvHandle, rHWLock.eDriverType);
                            gLockTimeOutCount = 0;
                            spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);
                            return -EFAULT;
                        }

                        // 2013/04/10. Cheng-Jung Never steal hardware lock
                        if (0)
                            //if (u4TimeInterval >= rHWLock.u4TimeoutMs)
                        {
                            if (rHWLock.eDriverType == VAL_DRIVER_TYPE_MP4_ENC)
                            {
                                MFV_LOGE("VCODEC_LOCKHW, Locked process(A) timeout, so release HW and let currnet process(B) to use HW\n");

                                // Set timeout status for previous lock owner.
                                spin_lock_irqsave(&OalHWContextLock, ulFlags);
                                ret = search_HWLockSlot_ByHandle(0, (VAL_HANDLE_T)grVcodecHWLock.pvHandle);
                                spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

                                if (ret == -1)
                                {
                                    MFV_LOGE("[ERROR] VCODEC_LOCKHW, Locked process (A) - Instance 0x%lx fail, didn't call InitHWLock \n",
                                             (VAL_ULONG_T)grVcodecHWLock.pvHandle);
                                    spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);
                                    return -EFAULT;
                                }
                                else
                                {
                                    spin_lock_irqsave(&OalHWContextLock, ulFlags);
                                    *((volatile VAL_UINT32_T *)oal_hw_context[ret].kva_u4HWIsCompleted) = 1;
                                    *((volatile VAL_UINT32_T *)oal_hw_context[ret].kva_u4HWIsTimeout) = 1;
                                    spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

                                    spin_lock_irqsave(&EncISRCountLock, ulFlagsISR);
                                    gu4EncISRCount++;
                                    spin_unlock_irqrestore(&EncISRCountLock, ulFlagsISR);
                                }

                                // Update lock owner to current
                                spin_lock_irqsave(&OalHWContextLock, ulFlags);
                                u4Index = search_HWLockSlot_ByTID(0, current->pid);
                                spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

                                if (u4Index == -1)
                                {
                                    MFV_LOGE("[ERROR] VCODEC_LOCKHW, Locked process - ID %d fail, didn't call InitHWLock\n", current->pid);
                                    spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);
                                    return -EFAULT;
                                }

                                grVcodecHWLock.pvHandle = (VAL_VOID_T *)pmem_user_v2p_video((unsigned long)rHWLock.pvHandle);
                                grVcodecHWLock.u4ThreadID = current->pid;
                                grVcodecHWLock.eDriverType = rHWLock.eDriverType;
                                spin_lock_irqsave(&OalHWContextLock, ulFlags);
                                oal_hw_context[u4Index].pvHandle = (VAL_HANDLE_T)grVcodecHWLock.pvHandle;
                                spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

                                eVideoGetTimeOfDay(&grVcodecHWLock.rLockedTime, sizeof(VAL_TIME_T));

                                MFV_LOGD("VCODEC_LOCKHW, LockInstance = 0x%lx, CurrentTID = %d, rLockedTime(s, us) = %d, %d\n",
                                         (VAL_ULONG_T)grVcodecHWLock.pvHandle, current->pid, grVcodecHWLock.rLockedTime.u4Sec, grVcodecHWLock.rLockedTime.u4uSec);

                                bLockedHW = VAL_TRUE;
                                venc_power_on();
                            }
                            else if (rHWLock.eDriverType == VAL_DRIVER_TYPE_H264_ENC ||
                                     rHWLock.eDriverType == VAL_DRIVER_TYPE_HEVC_ENC ||
                                     rHWLock.eDriverType == VAL_DRIVER_TYPE_JPEG_ENC)
                            {
                                grVcodecHWLock.pvHandle = (VAL_VOID_T *)pmem_user_v2p_video((VAL_ULONG_T)rHWLock.pvHandle);
                                grVcodecHWLock.u4ThreadID = current->pid;
                                grVcodecHWLock.eDriverType = rHWLock.eDriverType;
                                eVideoGetTimeOfDay(&grVcodecHWLock.rLockedTime, sizeof(VAL_TIME_T));

                                MFV_LOGD("VCODEC_LOCKHW, LockInstance = 0x%lx, CurrentTID = %d, rLockedTime(s, us) = %d, %d\n",
                                         (VAL_ULONG_T)grVcodecHWLock.pvHandle, current->pid, grVcodecHWLock.rLockedTime.u4Sec, grVcodecHWLock.rLockedTime.u4uSec);

                                bLockedHW = VAL_TRUE;
                                if (rHWLock.eDriverType == VAL_DRIVER_TYPE_H264_ENC ||
                                    rHWLock.eDriverType == VAL_DRIVER_TYPE_HEVC_ENC)
                                {
                                    venc_power_on();
                                }
                            }
                        }
                    }

                    if (VAL_TRUE == bLockedHW)
                    {
                        MFV_LOGE("VCODEC_LOCKHW, Lock ok grVcodecHWLock.pvHandle = 0x%lx, va:%lx, type:%d", (VAL_ULONG_T)grVcodecHWLock.pvHandle, (VAL_ULONG_T)rHWLock.pvHandle, rHWLock.eDriverType);
                        gLockTimeOutCount = 0;
                    }
                    spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);
                }

                if (VAL_FALSE == bLockedHW)
                {
                    MFV_LOGE("[ERROR] VCODEC_LOCKHW - ID %d  fail, someone locked HW already , 0x%lx, %lx, 0x%lx, type:%d\n", current->pid, (VAL_ULONG_T)grVcodecHWLock.pvHandle, pmem_user_v2p_video((VAL_ULONG_T)rHWLock.pvHandle), (VAL_ULONG_T)rHWLock.pvHandle, rHWLock.eDriverType);
                    gLockTimeOutCount = 0;
                    return -EFAULT;
                }

                spin_lock_irqsave(&LockEncHWCountLock, ulFlagsLockHW);
                gu4LockEncHWCount++;
                spin_unlock_irqrestore(&LockEncHWCountLock, ulFlagsLockHW);

                MFV_LOGD("VCODEC_LOCKHWed - tid = %d\n", current->pid);

                if (rHWLock.eDriverType == VAL_DRIVER_TYPE_MP4_ENC)
                {
                    // add for debugging checking
                    spin_lock_irqsave(&OalHWContextLock, ulFlags);
                    ret = search_HWLockSlot_ByTID(0, current->pid);
                    spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

                    if (ret == -1)
                    {
                        MFV_LOGE("VCODEC_LOCKHW - ID %d  fail, didn't call InitHWLock \n", current->pid);
                        return -EFAULT;
                    }
                }
            }
            else
            {
                MFV_LOGE("[WARNING] VCODEC_LOCKHW Unknown instance\n");
                return -EFAULT;
            }
            MFV_LOGD("VCODEC_LOCKHW - tid = %d\n", current->pid);
        }
        break;

        case VCODEC_UNLOCKHW:
        {
            MFV_LOGD("VCODEC_UNLOCKHW + tid = %d\n", current->pid);
            user_data_addr = (VAL_UINT8_T *)arg;
            ret = copy_from_user(&rHWLock, user_data_addr, sizeof(VAL_HW_LOCK_T));
            if (ret)
            {
                MFV_LOGE("[ERROR] VCODEC_UNLOCKHW, copy_from_user failed: %lu\n", ret);
                return -EFAULT;
            }

            MFV_LOGD("VCODEC_UNLOCKHW eDriverType = %d\n", rHWLock.eDriverType);
            eValRet = VAL_RESULT_INVALID_ISR;
            if (rHWLock.eDriverType == VAL_DRIVER_TYPE_MP4_DEC ||
                rHWLock.eDriverType == VAL_DRIVER_TYPE_HEVC_DEC ||
                rHWLock.eDriverType == VAL_DRIVER_TYPE_H264_DEC ||
                rHWLock.eDriverType == VAL_DRIVER_TYPE_MP1_MP2_DEC ||
                rHWLock.eDriverType == VAL_DRIVER_TYPE_VC1_DEC ||
                rHWLock.eDriverType == VAL_DRIVER_TYPE_VC1_ADV_DEC ||
                rHWLock.eDriverType == VAL_DRIVER_TYPE_VP8_DEC)
            {
                spin_lock_irqsave(&HWLock, ulFlagsHWLock);
                if (grVcodecHWLock.pvHandle == (VAL_VOID_T *)pmem_user_v2p_video((VAL_ULONG_T)rHWLock.pvHandle)) // Current owner give up hw lock
                {
                    grVcodecHWLock.pvHandle = 0;
                    grVcodecHWLock.eDriverType = VAL_DRIVER_TYPE_NONE;
                    //disable_irq(MT_VDEC_IRQ_ID);
                    disable_irq(VDEC_IRQ_ID);
                    // TODO: check if turning power off is ok
                    vdec_power_off();

#ifdef ENABLE_MMDVFS_VDEC
                    // MM DVFS related
                    if (VAL_TRUE == gMMDFVFSMonitorStarts && gMMDFVFSMonitorCounts > MONITOR_START_MINUS_1)
                    {
                        _monitor_duration = VdecDvfsGetMonitorDuration();
                        if (_monitor_duration < MONITOR_DURATION_MS)
                        {
                            _diff = VdecDvfsStep();
                            MFV_LOGD("[VCODEC][MMDVFS_VDEC] @@ UNLOCK - lock time(%d ms, %d ms), cnt=%d, _monitor_duration=%d", _diff, gHWLockInterval, gMMDFVFSMonitorCounts, _monitor_duration);
                        }
                        else
                        {
                            VdecDvfsStep();
                            _perc = (VAL_UINT32_T)(100 * gHWLockInterval / _monitor_duration);
                            MFV_LOGE("[VCODEC][MMDVFS_VDEC] @@ UNLOCK - reset monitor duration (%d ms), percent: %d, (DROP_PERCENTAGE = %d, RAISE_PERCENTAGE = %d)", _monitor_duration, _perc, DROP_PERCENTAGE, RAISE_PERCENTAGE);
                            if (_perc < DROP_PERCENTAGE)
                            {
                                SendDvfsRequest(DVFS_LOW);
                                VdecDvfsEnd(DVFS_LOW);
                            }
                            else if (_perc > RAISE_PERCENTAGE)
                            {
                                SendDvfsRequest(DVFS_HIGH);
                                VdecDvfsEnd(DVFS_HIGH);
                            }
                            else
                            {
                                VdecDvfsEnd(-1);
                            }
                        }
                    }
                    gMMDFVFSMonitorCounts ++;
#endif

                }
                else // Not current owner
                {
                    MFV_LOGE("[ERROR] VCODEC_UNLOCKHW, Not owner trying to unlock dec hardware 0x%lx\n", pmem_user_v2p_video((VAL_ULONG_T)rHWLock.pvHandle));
                    spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);
                    return -EFAULT;
                }
                spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);
                eValRet = eVideoSetEvent(&HWLockEvent, sizeof(VAL_EVENT_T));
            }
            else if (rHWLock.eDriverType == VAL_DRIVER_TYPE_H264_ENC ||
                     rHWLock.eDriverType == VAL_DRIVER_TYPE_HEVC_ENC ||
                     rHWLock.eDriverType == VAL_DRIVER_TYPE_JPEG_ENC ||
                     rHWLock.eDriverType == VAL_DRIVER_TYPE_MP4_ENC)
            {
                if (rHWLock.eDriverType == VAL_DRIVER_TYPE_MP4_ENC)
                {
                    // Debug
                    MFV_LOGE("Hybrid VCODEC_UNLOCKHW \n");
                }
                else if (rHWLock.eDriverType == VAL_DRIVER_TYPE_H264_ENC ||
                         rHWLock.eDriverType == VAL_DRIVER_TYPE_HEVC_ENC)
                {
                    spin_lock_irqsave(&HWLock, ulFlagsHWLock);
                    if (grVcodecHWLock.pvHandle == (VAL_VOID_T *)pmem_user_v2p_video((VAL_ULONG_T)rHWLock.pvHandle)) // Current owner give up hw lock
                    {
                        grVcodecHWLock.pvHandle = 0;
                        grVcodecHWLock.eDriverType = VAL_DRIVER_TYPE_NONE;
                        //disable_irq(MT_VENC_IRQ_ID);
                        disable_irq(VENC_IRQ_ID);
                        // turn venc power off
                        venc_power_off();
                    }
                    else // Not current owner
                    {
                        // [TODO] error handling
                        MFV_LOGE("[ERROR] VCODEC_UNLOCKHW, Not owner trying to unlock enc hardware 0x%lx, pa:%lx, va:%lx type:%d\n", (VAL_ULONG_T)grVcodecHWLock.pvHandle, pmem_user_v2p_video((VAL_ULONG_T)rHWLock.pvHandle), (VAL_ULONG_T)rHWLock.pvHandle, rHWLock.eDriverType);
                        spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);
                        return -EFAULT;
                    }
                    spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);
                    eValRet = eVideoSetEvent(&HWLockEvent, sizeof(VAL_EVENT_T));
                }
            }
            else
            {
                MFV_LOGE("[WARNING] VCODEC_UNLOCKHW Unknown instance\n");
                return -EFAULT;
            }
            MFV_LOGD("VCODEC_UNLOCKHW - tid = %d\n", current->pid);
        }
        break;

        case VCODEC_INC_PWR_USER:
        {
            MFV_LOGD("VCODEC_INC_PWR_USER + tid = %d\n", current->pid);
            user_data_addr = (VAL_UINT8_T *)arg;
            ret = copy_from_user(&rPowerParam, user_data_addr, sizeof(VAL_POWER_T));
            if (ret)
            {
                MFV_LOGE("[ERROR] VCODEC_INC_PWR_USER, copy_from_user failed: %lu\n", ret);
                return -EFAULT;
            }
            MFV_LOGD("[VCODEC] INC_PWR_USER eDriverType = %d\n", rPowerParam.eDriverType);
            mutex_lock(&L2CLock);

#ifdef VENC_USE_L2C
            if (rPowerParam.eDriverType == VAL_DRIVER_TYPE_H264_ENC)
            {
                gu4L2CCounter++;
                MFV_LOGD("[VCODEC] INC_PWR_USER L2C counter = %d\n", gu4L2CCounter);

                if (1 == gu4L2CCounter)
                {
                    if (config_L2(0))
                    {
                        MFV_LOGE("[VCODEC][ERROR] Switch L2C size to 512K failed\n");
                        mutex_unlock(&L2CLock);
                        return -EFAULT;
                    }
                    else
                    {
                        MFV_LOGE("[VCODEC] Switch L2C size to 512K successful\n");
                    }
                }
            }
#endif
            mutex_unlock(&L2CLock);
            MFV_LOGD("VCODEC_INC_PWR_USER - tid = %d\n", current->pid);
        }
        break;

        case VCODEC_DEC_PWR_USER:
        {
            MFV_LOGD("VCODEC_DEC_PWR_USER + tid = %d\n", current->pid);
            user_data_addr = (VAL_UINT8_T *)arg;
            ret = copy_from_user(&rPowerParam, user_data_addr, sizeof(VAL_POWER_T));
            if (ret)
            {
                MFV_LOGE("[ERROR] VCODEC_DEC_PWR_USER, copy_from_user failed: %lu\n", ret);
                return -EFAULT;
            }
            MFV_LOGD("[VCODEC] DEC_PWR_USER eDriverType = %d\n", rPowerParam.eDriverType);

            mutex_lock(&L2CLock);

#ifdef VENC_USE_L2C
            if (rPowerParam.eDriverType == VAL_DRIVER_TYPE_H264_ENC)
            {
                gu4L2CCounter--;
                MFV_LOGD("[VCODEC] DEC_PWR_USER L2C counter  = %d\n", gu4L2CCounter);

                if (0 == gu4L2CCounter)
                {
                    if (config_L2(1))
                    {
                        MFV_LOGE("[VCODEC][ERROR] Switch L2C size to 0K failed\n");
                        mutex_unlock(&L2CLock);
                        return -EFAULT;
                    }
                    else
                    {
                        MFV_LOGE("[VCODEC] Switch L2C size to 0K successful\n");
                    }
                }
            }
#endif
            mutex_unlock(&L2CLock);
            MFV_LOGD("VCODEC_DEC_PWR_USER - tid = %d\n", current->pid);
        }
        break;

        case VCODEC_WAITISR:
        {
            MFV_LOGD("VCODEC_WAITISR + tid = %d\n", current->pid);
            user_data_addr = (VAL_UINT8_T *)arg;
            ret = copy_from_user(&val_isr, user_data_addr, sizeof(VAL_ISR_T));
            if (ret)
            {
                MFV_LOGE("[ERROR] VCODEC_WAITISR, copy_from_user failed: %lu\n", ret);
                return -EFAULT;
            }

            if (val_isr.eDriverType == VAL_DRIVER_TYPE_MP4_DEC ||
                val_isr.eDriverType == VAL_DRIVER_TYPE_HEVC_DEC ||
                val_isr.eDriverType == VAL_DRIVER_TYPE_H264_DEC ||
                val_isr.eDriverType == VAL_DRIVER_TYPE_MP1_MP2_DEC ||
                val_isr.eDriverType == VAL_DRIVER_TYPE_VC1_DEC ||
                val_isr.eDriverType == VAL_DRIVER_TYPE_VC1_ADV_DEC ||
                val_isr.eDriverType == VAL_DRIVER_TYPE_VP8_DEC)
            {
                if(val_isr.eDriverType == VAL_DRIVER_TYPE_H264_DEC || val_isr.eDriverType == VAL_DRIVER_TYPE_VP8_DEC)
                {
                    MFV_LOGD("VCODEC_WAITISR_H264_DEC + tid = %d\n", current->pid);
                    spin_lock_irqsave(&OalHWContextLock, ulFlags);
                    //u4Index = search_HWLockSlot_ByHandle(0, pmem_user_v2p_video((unsigned long)val_isr.pvHandle));
                    //use search by thread ID, because handle may freed in user space
                    u4Index = search_HWLockSlot_ByTID(0, current->pid);
                    spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

                    if (u4Index == -1)
                    {
                        MFV_LOGE("[ERROR] VCODEC_WAITISR Fail, handle = 0x%lx, tid = %d, index = -1\n",
                                pmem_user_v2p_video((unsigned long)val_isr.pvHandle), current->pid);
                        return -EFAULT;
                    }

                    MFV_LOGD("VCODEC_WAITISR, index = %d, start wait VCODEC_WAITISR handle 0x%lx \n",
                            u4Index, pmem_user_v2p_video((unsigned long)val_isr.pvHandle));

                    //spin_lock_irqsave(&HWLock, ulFlagsHWLock);
                    //MFV_LOGD("VCODEC_WAITISR, grVcodecHWLock.pvHandle = 0x%lx\n",
                    //(VAL_ULONG_T)grVcodecHWLock.pvHandle);
                    ////spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);

                    spin_lock_irqsave(&OalHWContextLock, ulFlags);
                    oal_hw_context[u4Index].IsrEvent.u4TimeoutMs = val_isr.u4TimeoutMs;
                    spin_unlock_irqrestore(&OalHWContextLock, ulFlags);
                    eValRet = eVideoWaitEvent(&oal_hw_context[u4Index].IsrEvent, sizeof(VAL_EVENT_T));
                    MFV_LOGD("waitdone VCODEC_WAITISR handle 0x%lx \n",
                            pmem_user_v2p_video((unsigned long)val_isr.pvHandle));

                    if (VAL_RESULT_INVALID_ISR == eValRet)
                    {
                        if( DecIsrEvent.u4TimeoutMs == 1 && val_isr.eDriverType == VAL_DRIVER_TYPE_VP8_DEC) 
                        {
                            MFV_LOGE("[ERROR] VCODEC_WAITISR, VP8 intend to let first WAITISE fail with timeOut "
                                    "value = %d (%d)\n", DecIsrEvent.u4TimeoutMs, gu4VDecIRQCount);
                        }
                        else
                        {
                            MFV_LOGE("[ERROR] VCODEC_WAITISR, WAIT_ISR_CMD TimeOut (%d)\n", gu4VDecIRQCount);
                        }

                        spin_lock_irqsave(&OalHWContextLock, ulFlags);
                        *((volatile VAL_UINT32_T *)oal_hw_context[u4Index].kva_u4HWIsCompleted) = 0;
                        *((volatile VAL_UINT32_T *)oal_hw_context[u4Index].kva_u4HWIsTimeout) = 1;
                        spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

                        spin_lock_irqsave(&DecISRCountLock, ulFlagsISR);
                        gu4DecISRCount++;
                        spin_unlock_irqrestore(&DecISRCountLock, ulFlagsISR);

                        //Unlock
                        spin_lock_irqsave(&HWLock, ulFlagsHWLock);
                        //use search by thread ID to avoid handle already freed in user space that will cause KE!
                        for (i = 0; i < grVcodecHWLock.u4VCodecThreadNum; i++)
                        {
                            if (grVcodecHWLock.u4VCodecThreadID[i] == current->pid)
                                //if (grVcodecHWLock.pvHandle ==
                                //(VAL_VOID_T *)pmem_user_v2p_video((unsigned long)val_isr.pvHandle)) //normal case
                            {
                                for (j = 0; j < VCODEC_THREAD_MAX_NUM; j++)
                                {
                                    grVcodecHWLock.u4VCodecThreadID[j] = -1;
                                }
                                grVcodecHWLock.u4VCodecThreadNum = VCODEC_THREAD_MAX_NUM;
                                grVcodecHWLock.pvHandle = 0;
                                grVcodecHWLock.u4ThreadID = 0;
                                grVcodecHWLock.rLockedTime.u4Sec = 0;
                                grVcodecHWLock.rLockedTime.u4uSec = 0;
                                disable_irq_nosync(VDEC_IRQ_ID);
                                --gu4VDecIRQCount;
                                // turn venc power off
                                vdec_power_off();

                                eValHWLockRet = eVideoSetEvent(&HWLockEvent, sizeof(VAL_EVENT_T));
                                if (VAL_RESULT_NO_ERROR != eValHWLockRet)
                                {
                                    MFV_LOGE("[ERROR] VCODEC_WAITISR, set HWLockEvent error\n");
                                }
                                break;
                            }
                        }
                        spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);

                        // TODO: power down hw?
                        return -2;
                    }
                }
                else
                {
                    spin_lock_irqsave(&HWLock, ulFlagsHWLock);
                    if (grVcodecHWLock.pvHandle == (VAL_VOID_T *)pmem_user_v2p_video((VAL_ULONG_T)val_isr.pvHandle))
                    {
                        bLockedHW = VAL_TRUE;
                    }
                    else
                    {
                    }
                    spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);

                    if (bLockedHW == VAL_FALSE)
                    {
                        MFV_LOGE("[ERROR] VCODEC_WAITISR, DO NOT have HWLock, so return fail\n");
                        break;
                    }

                    spin_lock_irqsave(&DecIsrLock, ulFlags);
                    DecIsrEvent.u4TimeoutMs = val_isr.u4TimeoutMs;
                    spin_unlock_irqrestore(&DecIsrLock, ulFlags);

                    eValRet = eVideoWaitEvent(&DecIsrEvent, sizeof(VAL_EVENT_T));
                    if (VAL_RESULT_INVALID_ISR == eValRet)
                    {
                        return -2;
                    }
                    else if (VAL_RESULT_RESTARTSYS == eValRet)
                    {
                        MFV_LOGE("[WARNING] VCODEC_WAITISR, VAL_RESULT_RESTARTSYS return when WAITISR!!\n");
                        return -ERESTARTSYS;
                    }
                }
            }
            else if (val_isr.eDriverType == VAL_DRIVER_TYPE_H264_ENC ||
                     val_isr.eDriverType == VAL_DRIVER_TYPE_HEVC_ENC ||
                     val_isr.eDriverType == VAL_DRIVER_TYPE_MP4_ENC)
            {
                if (val_isr.eDriverType == VAL_DRIVER_TYPE_MP4_ENC) // Hybrid
                {
                    MFV_LOGD("VCODEC_WAITISR_MP4_ENC + tid = %d\n", current->pid);
                    spin_lock_irqsave(&OalHWContextLock, ulFlags);
                    //u4Index = search_HWLockSlot_ByHandle(0, pmem_user_v2p_video((unsigned long)val_isr.pvHandle));
                    //use search by thread ID, because handle may freed in user space
                    u4Index = search_HWLockSlot_ByTID(0, current->pid);
                    spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

                    if (u4Index == -1)
                    {
                        MFV_LOGE("[ERROR] VCODEC_WAITISR Fail, handle = 0x%lx, tid = %d, index = -1\n",
                                pmem_user_v2p_video((unsigned long)val_isr.pvHandle), current->pid);
                        return -EFAULT;
                    }

                    MFV_LOGD("VCODEC_WAITISR, index = %d, start wait VCODEC_WAITISR handle 0x%lx \n",
                            u4Index, pmem_user_v2p_video((unsigned long)val_isr.pvHandle));

                    //spin_lock_irqsave(&HWLock, ulFlagsHWLock);
                    //MFV_LOGD("VCODEC_WAITISR, grVcodecHWLock.pvHandle = 0x%lx\n",
                    //(VAL_ULONG_T)grVcodecHWLock.pvHandle);
                    ////spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);

                    spin_lock_irqsave(&OalHWContextLock, ulFlags);
                    oal_hw_context[u4Index].IsrEvent.u4TimeoutMs = val_isr.u4TimeoutMs;
                    spin_unlock_irqrestore(&OalHWContextLock, ulFlags);
                    eValRet = eVideoWaitEvent(&oal_hw_context[u4Index].IsrEvent, sizeof(VAL_EVENT_T));
                    MFV_LOGD("waitdone VCODEC_WAITISR handle 0x%lx \n",
                            pmem_user_v2p_video((unsigned long)val_isr.pvHandle));

                    if (VAL_RESULT_INVALID_ISR == eValRet)
                    {
                        MFV_LOGE("[ERROR] VCODEC_WAITISR, WAIT_ISR_CMD TimeOut\n");

                        spin_lock_irqsave(&OalHWContextLock, ulFlags);
                        *((volatile VAL_UINT32_T *)oal_hw_context[u4Index].kva_u4HWIsCompleted) = 0;
                        *((volatile VAL_UINT32_T *)oal_hw_context[u4Index].kva_u4HWIsTimeout) = 1;
                        spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

                        spin_lock_irqsave(&EncISRCountLock, ulFlagsISR);
                        gu4EncISRCount++;
                        spin_unlock_irqrestore(&EncISRCountLock, ulFlagsISR);

                        //Unlock
                        spin_lock_irqsave(&HWLock, ulFlagsHWLock);
                        //use search by thread ID to avoid handle already freed in user space
                        //that will cause KE!
                        for (i = 0; i < grVcodecHWLock.u4VCodecThreadNum; i++)
                        {
                            if (grVcodecHWLock.u4VCodecThreadID[i] == current->pid)
                                //if (grVcodecHWLock.pvHandle ==
                                //(VAL_VOID_T *)pmem_user_v2p_video((unsigned long)val_isr.pvHandle)) //normal case
                            {
                                for (j = 0; j < VCODEC_THREAD_MAX_NUM; j++)
                                {
                                    grVcodecHWLock.u4VCodecThreadID[j] = -1;
                                }
                                grVcodecHWLock.u4VCodecThreadNum = VCODEC_THREAD_MAX_NUM;
                                grVcodecHWLock.pvHandle = 0;
                                grVcodecHWLock.u4ThreadID = 0;
                                grVcodecHWLock.rLockedTime.u4Sec = 0;
                                grVcodecHWLock.rLockedTime.u4uSec = 0;
                                disable_irq_nosync(VENC_IRQ_ID);
                                --gu4VEncIRQCount;
                                // turn venc power off
                                venc_power_off();

                                eValHWLockRet = eVideoSetEvent(&HWLockEvent, sizeof(VAL_EVENT_T));
                                if (VAL_RESULT_NO_ERROR != eValHWLockRet)
                                {
                                    MFV_LOGE("[ERROR] VCODEC_WAITISR, set HWLockEvent error\n");
                                }
                                break;
                            }
                        }
                        spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);

                        // Cheng-Jung 20120614 Dump status register
#ifdef DEBUG_MP4_ENC
                        dump_venc_status();
#endif
                        // TODO: power down hw?
                        return -2;
                    }
                }
                else if (val_isr.eDriverType == VAL_DRIVER_TYPE_H264_ENC ||
                         val_isr.eDriverType == VAL_DRIVER_TYPE_VP8_ENC) // Pure HW
                {
                    spin_lock_irqsave(&HWLock, ulFlagsHWLock);
                    if (grVcodecHWLock.pvHandle == (VAL_VOID_T *)pmem_user_v2p_video((VAL_ULONG_T)val_isr.pvHandle))
                    {
                        bLockedHW = VAL_TRUE;
                    }
                    else
                    {
                    }
                    spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);

                    if (bLockedHW == VAL_FALSE)
                    {
                        MFV_LOGE("[ERROR] VCODEC_WAITISR, DO NOT have enc HWLock, so return fail pa:%lx, va:%lx\n", pmem_user_v2p_video((VAL_ULONG_T)val_isr.pvHandle), (VAL_ULONG_T)val_isr.pvHandle);
                        break;
                    }

                    spin_lock_irqsave(&EncIsrLock, ulFlags);
                    EncIsrEvent.u4TimeoutMs = val_isr.u4TimeoutMs;
                    spin_unlock_irqrestore(&EncIsrLock, ulFlags);

                    eValRet = eVideoWaitEvent(&EncIsrEvent, sizeof(VAL_EVENT_T));
                    if (VAL_RESULT_INVALID_ISR == eValRet)
                    {
                        return -2;
                    }
                    else if (VAL_RESULT_RESTARTSYS == eValRet)
                    {
                        MFV_LOGE("[WARNING] VCODEC_WAITISR, VAL_RESULT_RESTARTSYS return when WAITISR!!\n");
                        return -ERESTARTSYS;
                    }

                    if (val_isr.u4IrqStatusNum > 0)
                    {
                        val_isr.u4IrqStatus[0] = gu4HwVencIrqStatus;
                        ret = copy_to_user(user_data_addr, &val_isr, sizeof(VAL_ISR_T));
                        if (ret)
                        {
                            MFV_LOGE("[ERROR] VCODEC_WAITISR, copy_to_user failed: %lu\n", ret);
                            return -EFAULT;
                        }
                    }
                }
            }
            else
            {
                MFV_LOGE("[WARNING] VCODEC_WAITISR Unknown instance\n");
                return -EFAULT;
            }
            MFV_LOGD("VCODEC_WAITISR - tid = %d\n", current->pid);
        }
        break;

        case VCODEC_INITHWLOCK:
        {
            VAL_VCODEC_OAL_HW_CONTEXT_T *context;
            VAL_VCODEC_OAL_HW_REGISTER_T  hwoal_reg;
            VAL_VCODEC_OAL_HW_REGISTER_T  *kva_TempReg;
            VAL_VCODEC_OAL_MEM_STAUTS_T  oal_mem_status[OALMEM_STATUS_NUM];
            VAL_UINT32_T ret, i, pa_u4HWIsCompleted, pa_u4HWIsTimeout;
            VAL_ULONG_T addr_pa;

            MFV_LOGD("VCODEC_INITHWLOCK + - tid = %d\n", current->pid);

            ////////////// Start to get content
            ////////////// take VAL_VCODEC_OAL_HW_REGISTER_T content
            user_data_addr = (VAL_UINT8_T *)arg;
            ret = copy_from_user(&hwoal_reg, user_data_addr, sizeof(VAL_VCODEC_OAL_HW_REGISTER_T));

            // TODO:
#if IS_ENABLED(CONFIG_COMPAT)
            if (ori_user_data_addr != 0)
            {
                addr_pa = pmem_user_v2p_video((unsigned long)ori_user_data_addr);
            }
            else
#endif
            {
                addr_pa = pmem_user_v2p_video((unsigned long)user_data_addr);
            }

            spin_lock_irqsave(&OalHWContextLock, ulFlags);
            context = setCurr_HWLockSlot(addr_pa, current->pid);
            context->Oal_HW_reg = (VAL_VCODEC_OAL_HW_REGISTER_T *)arg;

#if IS_ENABLED(CONFIG_COMPAT)
            if (ori_pHWStatus != 0)
            {
                context->Oal_HW_mem_reg = ori_pHWStatus;
            }
            else
#endif
            {
                context->Oal_HW_mem_reg = (VAL_UINT32_T *)(((VAL_VCODEC_OAL_HW_REGISTER_T *)user_data_addr)->pHWStatus);
            }

            if (hwoal_reg.u4NumOfRegister != 0)
            {
                context->pa_Oal_HW_mem_reg =  pmem_user_v2p_video((unsigned long)context->Oal_HW_mem_reg);
            }
            pa_u4HWIsCompleted =  pmem_user_v2p_video((unsigned long) & (((VAL_VCODEC_OAL_HW_REGISTER_T *)user_data_addr)->u4HWIsCompleted));
            pa_u4HWIsTimeout =  pmem_user_v2p_video((unsigned long) & (((VAL_VCODEC_OAL_HW_REGISTER_T *)user_data_addr)->u4HWIsTimeout));
            MFV_LOGD("[VCODEC] user_data_addr->u4HWIsCompleted ua = 0x%lx pa= 0x%x\n", (unsigned long) & (((VAL_VCODEC_OAL_HW_REGISTER_T *)user_data_addr)->u4HWIsCompleted), pa_u4HWIsCompleted);
            MFV_LOGD("[VCODEC] user_data_addr->u4HWIsTimeout ua = 0x%lx pa= 0x%x\n", (unsigned long) & (((VAL_VCODEC_OAL_HW_REGISTER_T *)user_data_addr)->u4HWIsTimeout), pa_u4HWIsTimeout);

            //ret = copy_from_user(&oal_mem_status[0], ((VAL_VCODEC_OAL_HW_REGISTER_T *)user_data_addr)->pHWStatus, hwoal_reg.u4NumOfRegister*sizeof(VAL_VCODEC_OAL_MEM_STAUTS_T));
            ret = copy_from_user(&oal_mem_status[0], hwoal_reg.pHWStatus, hwoal_reg.u4NumOfRegister * sizeof(VAL_VCODEC_OAL_MEM_STAUTS_T));
            context->u4NumOfRegister = hwoal_reg.u4NumOfRegister;
            MFV_LOGW("[VCODEC_INITHWLOCK] ToTal %d u4NumOfRegister\n", hwoal_reg.u4NumOfRegister);

            if (hwoal_reg.u4NumOfRegister != 0)
            {
                u8TempKPA = context->pa_Oal_HW_mem_reg;
                spin_unlock_irqrestore(&OalHWContextLock, ulFlags);
                mutex_lock(&NonCacheMemoryListLock);
                pu4TempKVA = (VAL_UINT32_T *)Search_NonCacheMemoryList_By_KPA(u8TempKPA);
                mutex_unlock(&NonCacheMemoryListLock);
                spin_lock_irqsave(&OalHWContextLock, ulFlags);
                context->kva_Oal_HW_mem_reg = pu4TempKVA;
                MFV_LOGD("[VCODEC] context->ua = 0x%lx  pa_Oal_HW_mem_reg = 0x%lx \n", (VAL_ULONG_T)context->Oal_HW_mem_reg, context->pa_Oal_HW_mem_reg);
            }
            spin_unlock_irqrestore(&OalHWContextLock, ulFlags);
            mutex_lock(&NonCacheMemoryListLock);
            kva_TempReg = (VAL_VCODEC_OAL_HW_REGISTER_T *)Search_NonCacheMemoryList_By_KPA(addr_pa);
            mutex_unlock(&NonCacheMemoryListLock);
            spin_lock_irqsave(&OalHWContextLock, ulFlags);
            context->kva_u4HWIsCompleted = (VAL_ULONG_T)(&(kva_TempReg->u4HWIsCompleted));
            context->kva_u4HWIsTimeout = (VAL_ULONG_T)(&(kva_TempReg->u4HWIsTimeout));
            MFV_LOGD("[VCODEC] kva_TempReg = 0x%lx, kva_u4HWIsCompleted = 0x%lx, kva_u4HWIsTimeout = 0x%lx\n",
                     (VAL_ULONG_T)kva_TempReg, context->kva_u4HWIsCompleted, context->kva_u4HWIsTimeout);

            for (i = 0; i < hwoal_reg.u4NumOfRegister ; i++)
            {
                VAL_ULONG_T kva;
                MFV_LOGE("[VCODEC][REG_INFO_1] [%d] 0x%lx 0x%x \n", i ,
                         oal_mem_status[i].u4ReadAddr, oal_mem_status[i].u4ReadData);

                addr_pa = pmem_user_v2p_video((unsigned long)oal_mem_status[i].u4ReadAddr);
                spin_unlock_irqrestore(&OalHWContextLock, ulFlags);
                kva = (VAL_ULONG_T)ioremap(addr_pa, 8); // need to remap addr + data addr
                spin_lock_irqsave(&OalHWContextLock, ulFlags);
                MFV_LOGE("[VCODEC][REG_INFO_2] [%d] pa = 0x%lx  kva = 0x%lx \n", i , addr_pa, kva);
                context->oalmem_status[i].u4ReadAddr = kva; //oal_mem_status[i].u4ReadAddr;
            }
            spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

            MFV_LOGD("VCODEC_INITHWLOCK addr1 0x%lx addr2 0x%lx \n",
                     (unsigned long)arg, (unsigned long)context->Oal_HW_mem_reg);
            MFV_LOGD("VCODEC_INITHWLOCK - - tid = %d\n", current->pid);
        }
        break;

        case VCODEC_DEINITHWLOCK:
        {
            VAL_UINT8_T *user_data_addr;
            VAL_ULONG_T addr_pa;

            MFV_LOGD("VCODEC_DEINITHWLOCK + - tid = %d\n", current->pid);

            user_data_addr = (VAL_UINT8_T *)arg;
            // TODO:
            addr_pa = pmem_user_v2p_video((unsigned long)user_data_addr);

            spin_lock_irqsave(&OalHWContextLock, ulFlags);
            freeCurr_HWLockSlot(addr_pa);
            spin_unlock_irqrestore(&OalHWContextLock, ulFlags);
            MFV_LOGD("VCODEC_DEINITHWLOCK - - tid = %d\n", current->pid);
        }
        break;

        case VCODEC_GET_CPU_LOADING_INFO:
        {
            VAL_UINT8_T *user_data_addr;
            VAL_VCODEC_CPU_LOADING_INFO_T _temp;

            MFV_LOGD("VCODEC_GET_CPU_LOADING_INFO +\n");
            user_data_addr = (VAL_UINT8_T *)arg;
            // TODO:
#if 0 // Morris Yang 20120112 mark temporarily
            _temp._cpu_idle_time = mt_get_cpu_idle(0);
            _temp._thread_cpu_time = mt_get_thread_cputime(0);
            spin_lock_irqsave(&OalHWContextLock, ulFlags);
            _temp._inst_count = getCurInstanceCount();
            spin_unlock_irqrestore(&OalHWContextLock, ulFlags);
            _temp._sched_clock = mt_sched_clock();
#endif
            ret = copy_to_user(user_data_addr, &_temp, sizeof(VAL_VCODEC_CPU_LOADING_INFO_T));
            if (ret)
            {
                MFV_LOGE("[ERROR] VCODEC_GET_CPU_LOADING_INFO, copy_to_user failed: %lu\n", ret);
                return -EFAULT;
            }

            MFV_LOGD("VCODEC_GET_CPU_LOADING_INFO -\n");
        }
        break;

        case VCODEC_GET_CORE_LOADING:
        {
            MFV_LOGD("VCODEC_GET_CORE_LOADING + - tid = %d\n", current->pid);

            user_data_addr = (VAL_UINT8_T *)arg;
            ret = copy_from_user(&rTempCoreLoading, user_data_addr, sizeof(VAL_VCODEC_CORE_LOADING_T));
            if (ret)
            {
                MFV_LOGE("[ERROR] VCODEC_GET_CORE_LOADING, copy_from_user failed: %lu\n", ret);
                return -EFAULT;
            }
            rTempCoreLoading.Loading = get_cpu_load(rTempCoreLoading.CPUid);
            ret = copy_to_user(user_data_addr, &rTempCoreLoading, sizeof(VAL_VCODEC_CORE_LOADING_T));
            if (ret)
            {
                MFV_LOGE("[ERROR] VCODEC_GET_CORE_LOADING, copy_to_user failed: %lu\n", ret);
                return -EFAULT;
            }

            MFV_LOGD("VCODEC_GET_CORE_LOADING - - tid = %d\n", current->pid);
        }
        break;

        case VCODEC_GET_CORE_NUMBER:
        {
            MFV_LOGD("VCODEC_GET_CORE_NUMBER + - tid = %d\n", current->pid);

            user_data_addr = (VAL_UINT8_T *)arg;
            temp_nr_cpu_ids = nr_cpu_ids;
            ret = copy_to_user(user_data_addr, &temp_nr_cpu_ids, sizeof(int));
            if (ret)
            {
                MFV_LOGE("[ERROR] VCODEC_GET_CORE_NUMBER, copy_to_user failed: %lu\n", ret);
                return -EFAULT;
            }
            MFV_LOGD("VCODEC_GET_CORE_NUMBER - - tid = %d\n", current->pid);
        }
        break;

        case VCODEC_SET_CPU_OPP_LIMIT:
        {
            MFV_LOGE("VCODEC_SET_CPU_OPP_LIMIT [EMPTY] + - tid = %d\n", current->pid);
            user_data_addr = (VAL_UINT8_T *)arg;
            ret = copy_from_user(&rCpuOppLimit, user_data_addr, sizeof(VAL_VCODEC_CPU_OPP_LIMIT_T));
            if (ret)
            {
                MFV_LOGE("[ERROR] VCODEC_SET_CPU_OPP_LIMIT, copy_from_user failed: %lu\n", ret);
                return -EFAULT;
            }
            MFV_LOGE("+VCODEC_SET_CPU_OPP_LIMIT (%d, %d, %d), tid = %d\n", rCpuOppLimit.limited_freq, rCpuOppLimit.limited_cpu, rCpuOppLimit.enable, current->pid);
            // TODO: Check if cpu_opp_limit is available
            //ret = cpu_opp_limit(EVENT_VIDEO, rCpuOppLimit.limited_freq, rCpuOppLimit.limited_cpu, rCpuOppLimit.enable); // 0: PASS, other: FAIL
            if (ret)
            {
                MFV_LOGE("[VCODEC][ERROR] cpu_opp_limit failed: %lu\n", ret);
                return -EFAULT;
            }
            MFV_LOGE("-VCODEC_SET_CPU_OPP_LIMIT tid = %d, ret = %lu\n", current->pid, ret);
            MFV_LOGE("VCODEC_SET_CPU_OPP_LIMIT [EMPTY] - - tid = %d\n", current->pid);
        }
        break;

        case VCODEC_MB:
        {
            mb();
        }
        break;
#if 0
        case MFV_SET_CMD_CMD:
            MFV_LOGD("[MFV] MFV_SET_CMD_CMD\n");
            MFV_LOGD("[MFV] Arg = %x\n", arg);
            user_data_addr = (VAL_UINT8_T *)arg;
            ret = copy_from_user(&rDrvCmdQueue, user_data_addr, sizeof(VCODEC_DRV_CMD_QUEUE_T));
            MFV_LOGD("[MFV] CmdNum = %d\n", rDrvCmdQueue.CmdNum);
            u4Size = (rDrvCmdQueue.CmdNum) * sizeof(VCODEC_DRV_CMD_T);

            cmd_queue = (P_VCODEC_DRV_CMD_T)kmalloc(u4Size, GFP_ATOMIC);
            if (cmd_queue != VAL_NULL && rDrvCmdQueue.pCmd != VAL_NULL)
            {
                ret = copy_from_user(cmd_queue, rDrvCmdQueue.pCmd, u4Size);
                while (cmd_queue->type != END_CMD)
                {
                    switch (cmd_queue->type)
                    {
                        case ENABLE_HW_CMD:
                            break;
                        case DISABLE_HW_CMD:
                            break;
                        case WRITE_REG_CMD:
                            VDO_HW_WRITE(cmd_queue->address + cmd_queue->offset, cmd_queue->value);
                            break;
                        case READ_REG_CMD:
                            uValue = VDO_HW_READ(cmd_queue->address + cmd_queue->offset);
                            copy_to_user((void *)cmd_queue->value, &uValue, sizeof(VAL_UINT32_T));
                            break;
                        case WRITE_SYSRAM_CMD:
                            VDO_HW_WRITE(cmd_queue->address + cmd_queue->offset, cmd_queue->value);
                            break;
                        case READ_SYSRAM_CMD:
                            uValue = VDO_HW_READ(cmd_queue->address + cmd_queue->offset);
                            copy_to_user((void *)cmd_queue->value, &uValue, sizeof(VAL_UINT32_T));
                            break;
                        case MASTER_WRITE_CMD:
                            uValue = VDO_HW_READ(cmd_queue->address + cmd_queue->offset);
                            VDO_HW_WRITE(cmd_queue->address + cmd_queue->offset, cmd_queue->value | (uValue & cmd_queue->mask));
                            break;
                        case SETUP_ISR_CMD:
                            break;
                        case WAIT_ISR_CMD:
                            MFV_LOGD("HAL_CMD_SET_CMD_QUEUE: WAIT_ISR_CMD+\n");

                            MFV_LOGD("HAL_CMD_SET_CMD_QUEUE: WAIT_ISR_CMD-\n");
                            break;
                        case TIMEOUT_CMD:
                            break;
                        case WRITE_SYSRAM_RANGE_CMD:
                            break;
                        case READ_SYSRAM_RANGE_CMD:
                            break;
                        case POLL_REG_STATUS_CMD:
                            uValue = VDO_HW_READ(cmd_queue->address + cmd_queue->offset);
                            nCount = 0;
                            while ((uValue & cmd_queue->mask) != 0)
                            {
                                nCount++;
                                if (nCount > 1000)
                                {
                                    break;
                                }
                                uValue = VDO_HW_READ(cmd_queue->address + cmd_queue->offset);
                            }
                            break;
                        default:
                            break;
                    }
                    cmd_queue++;
                }

            }
            break;
#endif
        default:
        {
            MFV_LOGE("========[ERROR] vcodec_ioctl default case======== %u\n", cmd);
        }
        break;
    }
    return 0xFF;
}

#if IS_ENABLED(CONFIG_COMPAT)

typedef enum
{
    VAL_HW_LOCK_TYPE = 0,
    VAL_POWER_TYPE,
    VAL_ISR_TYPE,
    VAL_MEMORY_TYPE,
    VAL_VCODEC_OAL_HW_REGISTER_TYPE
} STRUCT_TYPE;

typedef enum
{
    COPY_FROM_USER = 0,
    COPY_TO_USER,
} COPY_DIRECTION;

typedef struct COMPAT_VAL_HW_LOCK
{
    compat_uptr_t       pvHandle;              ///< [IN]     The video codec driver handle
    compat_uint_t       u4HandleSize;           ///< [IN]     The size of video codec driver handle
    compat_uptr_t       pvLock;                ///< [IN/OUT] The Lock discriptor
    compat_uint_t       u4TimeoutMs;            ///< [IN]     The timeout ms
    compat_uptr_t       pvReserved;            ///< [IN/OUT] The reserved parameter
    compat_uint_t       u4ReservedSize;         ///< [IN]     The size of reserved parameter structure
    compat_uint_t       eDriverType;            ///< [IN]     The driver type
    char                bSecureInst;            ///< [IN]     True if this is a secure instance // MTK_SEC_VIDEO_PATH_SUPPORT
} COMPAT_VAL_HW_LOCK_T;

typedef struct COMPAT_VAL_POWER
{
    compat_uptr_t       pvHandle;              ///< [IN]     The video codec driver handle
    compat_uint_t       u4HandleSize;           ///< [IN]     The size of video codec driver handle
    compat_uint_t       eDriverType;            ///< [IN]     The driver type
    char                fgEnable;               ///< [IN]     Enable or not.
    compat_uptr_t       pvReserved;            ///< [IN/OUT] The reserved parameter
    compat_uint_t       u4ReservedSize;         ///< [IN]     The size of reserved parameter structure
    //VAL_UINT32_T        u4L2CUser;              ///< [OUT]    The number of power user right now
} COMPAT_VAL_POWER_T;

typedef struct COMPAT_VAL_ISR
{
    compat_uptr_t       pvHandle;              ///< [IN]     The video codec driver handle
    compat_uint_t       u4HandleSize;           ///< [IN]     The size of video codec driver handle
    compat_uint_t       eDriverType;            ///< [IN]     The driver type
    compat_uptr_t       pvIsrFunction;         ///< [IN]     The isr function
    compat_uptr_t       pvReserved;            ///< [IN/OUT] The reserved parameter
    compat_uint_t       u4ReservedSize;         ///< [IN]     The size of reserved parameter structure
    compat_uint_t       u4TimeoutMs;            ///< [IN]     The timeout in ms
    compat_uint_t       u4IrqStatusNum;                     ///< [IN]     The num of return registers when HW done
    compat_uint_t       u4IrqStatus[IRQ_STATUS_MAX_NUM];    ///< [IN/OUT] The value of return registers when HW done
} COMPAT_VAL_ISR_T;

typedef struct COMPAT_VAL_MEMORY
{
    compat_uint_t       eMemType;                   ///< [IN]     The allocation memory type
    compat_ulong_t      u4MemSize;                  ///< [IN]     The size of memory allocation
    compat_uptr_t       pvMemVa;                   ///< [IN/OUT] The memory virtual address
    compat_uptr_t       pvMemPa;                   ///< [IN/OUT] The memory physical address
    compat_uint_t       eAlignment;                 ///< [IN]     The memory byte alignment setting
    compat_uptr_t       pvAlignMemVa;              ///< [IN/OUT] The align memory virtual address
    compat_uptr_t       pvAlignMemPa;              ///< [IN/OUT] The align memory physical address
    compat_uint_t       eMemCodec;                  ///< [IN]     The memory codec for VENC or VDEC
    compat_uint_t       i4IonShareFd;
    compat_uptr_t       pIonBufhandle;
    compat_uptr_t       pvReserved;                ///< [IN/OUT] The reserved parameter
    compat_ulong_t      u4ReservedSize;             ///< [IN]     The size of reserved parameter structure
} COMPAT_VAL_MEMORY_T;


typedef struct COMPAT_VAL_VCODEC_OAL_HW_REGISTER
{
    compat_uint_t    u4HWIsCompleted;            ///< [IN/OUT] HW is Completed or not, set by driver & clear by codec (0: not completed or still in lock status; 1: HW is completed or in unlock status)
    compat_uint_t    u4HWIsTimeout;              ///< [OUT]    HW is Timeout or not, set by driver & clear by codec (0: not in timeout status; 1: HW is in timeout status)
    compat_uint_t    u4NumOfRegister;            ///< [IN]     Number of HW register need to store;
    compat_uptr_t    pHWStatus;     ///< [OUT]    HW status based on input address.
} COMPAT_VAL_VCODEC_OAL_HW_REGISTER_T;

typedef struct COMPAT_VAL_VCODEC_OAL_MEM_STAUTS
{
    compat_ulong_t  u4ReadAddr;                 /// [IN]  memory source address in VA
    compat_uint_t   u4ReadData;                 /// [OUT] memory data
} COMPAT_VAL_VCODEC_OAL_MEM_STAUTS_T;

static int compat_copy_struct(
    STRUCT_TYPE eType,
    COPY_DIRECTION eDirection,
    void __user *data32,
    void __user *data)
{
    compat_uint_t u;
    compat_ulong_t l;
    compat_uptr_t p;
    char c;
    int err = 0;

    switch (eType)
    {
        case VAL_HW_LOCK_TYPE:
        {
            if (eDirection == COPY_FROM_USER)
            {
                COMPAT_VAL_HW_LOCK_T __user *from32 = (COMPAT_VAL_HW_LOCK_T *)data32;
                VAL_HW_LOCK_T __user *to = (VAL_HW_LOCK_T *)data;

                err = get_user(p, &(from32->pvHandle));
                err |= put_user(p, &(to->pvHandle));
                err |= get_user(u, &(from32->u4HandleSize));
                err |= put_user(u, &(to->u4HandleSize));
                err |= get_user(p, &(from32->pvLock));
                err |= put_user(p, &(to->pvLock));
                err |= get_user(u, &(from32->u4TimeoutMs));
                err |= put_user(u, &(to->u4TimeoutMs));
                err |= get_user(p, &(from32->pvReserved));
                err |= put_user(p, &(to->pvReserved));
                err |= get_user(u, &(from32->u4ReservedSize));
                err |= put_user(u, &(to->u4ReservedSize));
                err |= get_user(u, &(from32->eDriverType));
                err |= put_user(u, &(to->eDriverType));
                err |= get_user(c, &(from32->bSecureInst));
                err |= put_user(c, &(to->bSecureInst));
            }
            else
            {
                COMPAT_VAL_HW_LOCK_T __user *to32 = (COMPAT_VAL_HW_LOCK_T *)data32;
                VAL_HW_LOCK_T __user *from = (VAL_HW_LOCK_T *)data;

                err = get_user(p, &(from->pvHandle));
                err |= put_user(p, &(to32->pvHandle));
                err |= get_user(u, &(from->u4HandleSize));
                err |= put_user(u, &(to32->u4HandleSize));
                err |= get_user(p, &(from->pvLock));
                err |= put_user(p, &(to32->pvLock));
                err |= get_user(u, &(from->u4TimeoutMs));
                err |= put_user(u, &(to32->u4TimeoutMs));
                err |= get_user(p, &(from->pvReserved));
                err |= put_user(p, &(to32->pvReserved));
                err |= get_user(u, &(from->u4ReservedSize));
                err |= put_user(u, &(to32->u4ReservedSize));
                err |= get_user(u, &(from->eDriverType));
                err |= put_user(u, &(to32->eDriverType));
                err |= get_user(c, &(from->bSecureInst));
                err |= put_user(c, &(to32->bSecureInst));
            }
        }
        break;
        case VAL_POWER_TYPE:
        {
            if (eDirection == COPY_FROM_USER)
            {
                COMPAT_VAL_POWER_T __user *from32 = (COMPAT_VAL_POWER_T *)data32;
                VAL_POWER_T __user *to = (VAL_POWER_T *)data;

                err = get_user(p, &(from32->pvHandle));
                err |= put_user(p, &(to->pvHandle));
                err |= get_user(u, &(from32->u4HandleSize));
                err |= put_user(u, &(to->u4HandleSize));
                err |= get_user(u, &(from32->eDriverType));
                err |= put_user(u, &(to->eDriverType));
                err |= get_user(c, &(from32->fgEnable));
                err |= put_user(c, &(to->fgEnable));
                err |= get_user(p, &(from32->pvReserved));
                err |= put_user(p, &(to->pvReserved));
                err |= get_user(u, &(from32->u4ReservedSize));
                err |= put_user(u, &(to->u4ReservedSize));
            }
            else
            {
                COMPAT_VAL_POWER_T __user *to32 = (COMPAT_VAL_POWER_T *)data32;
                VAL_POWER_T __user *from = (VAL_POWER_T *)data;

                err = get_user(p, &(from->pvHandle));
                err |= put_user(p, &(to32->pvHandle));
                err |= get_user(u, &(from->u4HandleSize));
                err |= put_user(u, &(to32->u4HandleSize));
                err |= get_user(u, &(from->eDriverType));
                err |= put_user(u, &(to32->eDriverType));
                err |= get_user(c, &(from->fgEnable));
                err |= put_user(c, &(to32->fgEnable));
                err |= get_user(p, &(from->pvReserved));
                err |= put_user(p, &(to32->pvReserved));
                err |= get_user(u, &(from->u4ReservedSize));
                err |= put_user(u, &(to32->u4ReservedSize));
            }
        }
        break;
        case VAL_ISR_TYPE:
        {
            int i = 0;
            if (eDirection == COPY_FROM_USER)
            {
                COMPAT_VAL_ISR_T __user *from32 = (COMPAT_VAL_ISR_T *)data32;
                VAL_ISR_T __user *to = (VAL_ISR_T *)data;

                err = get_user(p, &(from32->pvHandle));
                err |= put_user(p, &(to->pvHandle));
                err |= get_user(u, &(from32->u4HandleSize));
                err |= put_user(u, &(to->u4HandleSize));
                err |= get_user(u, &(from32->eDriverType));
                err |= put_user(u, &(to->eDriverType));
                err |= get_user(p, &(from32->pvIsrFunction));
                err |= put_user(p, &(to->pvIsrFunction));
                err |= get_user(p, &(from32->pvReserved));
                err |= put_user(p, &(to->pvReserved));
                err |= get_user(u, &(from32->u4ReservedSize));
                err |= put_user(u, &(to->u4ReservedSize));
                err |= get_user(u, &(from32->u4TimeoutMs));
                err |= put_user(u, &(to->u4TimeoutMs));
                err |= get_user(u, &(from32->u4IrqStatusNum));
                err |= put_user(u, &(to->u4IrqStatusNum));
                for (; i < IRQ_STATUS_MAX_NUM; i++)
                {
                    err |= get_user(u, &(from32->u4IrqStatus[i]));
                    err |= put_user(u, &(to->u4IrqStatus[i]));
                }
            }
            else
            {
                COMPAT_VAL_ISR_T __user *to32 = (COMPAT_VAL_ISR_T *)data32;
                VAL_ISR_T __user *from = (VAL_ISR_T *)data;

                err = get_user(p, &(from->pvHandle));
                err |= put_user(p, &(to32->pvHandle));
                err |= get_user(u, &(from->u4HandleSize));
                err |= put_user(u, &(to32->u4HandleSize));
                err |= get_user(u, &(from->eDriverType));
                err |= put_user(u, &(to32->eDriverType));
                err |= get_user(p, &(from->pvIsrFunction));
                err |= put_user(p, &(to32->pvIsrFunction));
                err |= get_user(p, &(from->pvReserved));
                err |= put_user(p, &(to32->pvReserved));
                err |= get_user(u, &(from->u4ReservedSize));
                err |= put_user(u, &(to32->u4ReservedSize));
                err |= get_user(u, &(from->u4TimeoutMs));
                err |= put_user(u, &(to32->u4TimeoutMs));
                err |= get_user(u, &(from->u4IrqStatusNum));
                err |= put_user(u, &(to32->u4IrqStatusNum));
                for (; i < IRQ_STATUS_MAX_NUM; i++)
                {
                    err |= get_user(u, &(from->u4IrqStatus[i]));
                    err |= put_user(u, &(to32->u4IrqStatus[i]));
                }
            }
        }
        break;
        case VAL_MEMORY_TYPE:
        {
            if (eDirection == COPY_FROM_USER)
            {
                COMPAT_VAL_MEMORY_T __user *from32 = (COMPAT_VAL_MEMORY_T *)data32;
                VAL_MEMORY_T __user *to = (VAL_MEMORY_T *)data;

                err = get_user(u, &(from32->eMemType));
                err |= put_user(u, &(to->eMemType));
                err |= get_user(l, &(from32->u4MemSize));
                err |= put_user(l, &(to->u4MemSize));
                err |= get_user(p, &(from32->pvMemVa));
                err |= put_user(p, &(to->pvMemVa));
                err |= get_user(p, &(from32->pvMemPa));
                err |= put_user(p, &(to->pvMemPa));
                err |= get_user(u, &(from32->eAlignment));
                err |= put_user(u, &(to->eAlignment));
                err |= get_user(p, &(from32->pvAlignMemVa));
                err |= put_user(p, &(to->pvAlignMemVa));
                err |= get_user(p, &(from32->pvAlignMemPa));
                err |= put_user(p, &(to->pvAlignMemPa));
                err |= get_user(u, &(from32->eMemCodec));
                err |= put_user(u, &(to->eMemCodec));
                err |= get_user(u, &(from32->i4IonShareFd));
                err |= put_user(u, &(to->i4IonShareFd));
                err |= get_user(p, &(from32->pIonBufhandle));
                err |= put_user(p, &(to->pIonBufhandle));
                err |= get_user(p, &(from32->pvReserved));
                err |= put_user(p, &(to->pvReserved));
                err |= get_user(l, &(from32->u4ReservedSize));
                err |= put_user(l, &(to->u4ReservedSize));
            }
            else
            {
                COMPAT_VAL_MEMORY_T __user *to32 = (COMPAT_VAL_MEMORY_T *)data32;
                VAL_MEMORY_T __user *from = (VAL_MEMORY_T *)data;

                err = get_user(u, &(from->eMemType));
                err |= put_user(u, &(to32->eMemType));
                err |= get_user(l, &(from->u4MemSize));
                err |= put_user(l, &(to32->u4MemSize));
                err |= get_user(p, &(from->pvMemVa));
                err |= put_user(p, &(to32->pvMemVa));
                err |= get_user(p, &(from->pvMemPa));
                err |= put_user(p, &(to32->pvMemPa));
                err |= get_user(u, &(from->eAlignment));
                err |= put_user(u, &(to32->eAlignment));
                err |= get_user(p, &(from->pvAlignMemVa));
                err |= put_user(p, &(to32->pvAlignMemVa));
                err |= get_user(p, &(from->pvAlignMemPa));
                err |= put_user(p, &(to32->pvAlignMemPa));
                err |= get_user(u, &(from->eMemCodec));
                err |= put_user(u, &(to32->eMemCodec));
                err |= get_user(u, &(from->i4IonShareFd));
                err |= put_user(u, &(to32->i4IonShareFd));
                err |= get_user(p, &(from->pIonBufhandle));
                err |= put_user(p, &(to32->pIonBufhandle));
                err |= get_user(p, &(from->pvReserved));
                err |= put_user(p, &(to32->pvReserved));
                err |= get_user(l, &(from->u4ReservedSize));
                err |= put_user(l, &(to32->u4ReservedSize));
            }
        }
        break;
        case VAL_VCODEC_OAL_HW_REGISTER_TYPE:
        {
            if (eDirection == COPY_FROM_USER)
            {
                COMPAT_VAL_VCODEC_OAL_HW_REGISTER_T __user *from32 = (COMPAT_VAL_VCODEC_OAL_HW_REGISTER_T *)data32;
                VAL_VCODEC_OAL_HW_REGISTER_T __user *to = (VAL_VCODEC_OAL_HW_REGISTER_T *)data;

                err = get_user(u, &(from32->u4HWIsCompleted));
                err |= put_user(u, &(to->u4HWIsCompleted));
                err |= get_user(u, &(from32->u4HWIsTimeout));
                err |= put_user(u, &(to->u4HWIsTimeout));
                err |= get_user(u, &(from32->u4NumOfRegister));
                err |= put_user(u, &(to->u4NumOfRegister));
                err |= get_user(p, &(from32->pHWStatus));
                err |= put_user(p, &(to->pHWStatus));
            }
            else
            {
                COMPAT_VAL_VCODEC_OAL_HW_REGISTER_T __user *to32 = (COMPAT_VAL_VCODEC_OAL_HW_REGISTER_T *)data32;
                VAL_VCODEC_OAL_HW_REGISTER_T __user *from = (VAL_VCODEC_OAL_HW_REGISTER_T *)data;

                err = get_user(u, &(from->u4HWIsCompleted));
                err |= put_user(u, &(to32->u4HWIsCompleted));
                err |= get_user(u, &(from->u4HWIsTimeout));
                err |= put_user(u, &(to32->u4HWIsTimeout));
                err |= get_user(u, &(from->u4NumOfRegister));
                err |= put_user(u, &(to32->u4NumOfRegister));
                err |= get_user(p, &(from->pHWStatus));
                err |= put_user(p, &(to32->pHWStatus));
            }
        }
        break;
        default:
            break;
    }

    return err;
}


static long vcodec_unlocked_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    long ret = 0;
    MFV_LOGE("vcodec_unlocked_compat_ioctl: 0x%x\n", cmd);
    switch (cmd)
    {
        case VCODEC_ALLOC_NON_CACHE_BUFFER:
        case VCODEC_FREE_NON_CACHE_BUFFER:
        {
            COMPAT_VAL_MEMORY_T __user *data32;
            VAL_MEMORY_T __user *data;
            int err;

            data32 = compat_ptr(arg);
            data = compat_alloc_user_space(sizeof(VAL_MEMORY_T));
            if (data == NULL)
            {
                return -EFAULT;
            }

            err = compat_copy_struct(VAL_MEMORY_TYPE, COPY_FROM_USER, (void *)data32, (void *)data);
            if (err)
            {
                return err;
            }

            ret = file->f_op->unlocked_ioctl(file, cmd, (unsigned long)data);

            err = compat_copy_struct(VAL_MEMORY_TYPE, COPY_TO_USER, (void *)data32, (void *)data);

            if (err)
            {
                return err;
            }
            return ret;
        }
        break;
        case VCODEC_LOCKHW:
        case VCODEC_UNLOCKHW:
        {
            COMPAT_VAL_HW_LOCK_T __user *data32;
            VAL_HW_LOCK_T __user *data;
            int err;

            data32 = compat_ptr(arg);
            data = compat_alloc_user_space(sizeof(VAL_HW_LOCK_T));
            if (data == NULL)
            {
                return -EFAULT;
            }

            err = compat_copy_struct(VAL_HW_LOCK_TYPE, COPY_FROM_USER, (void *)data32, (void *)data);
            if (err)
            {
                return err;
            }

            ret = file->f_op->unlocked_ioctl(file, cmd, (unsigned long)data);

            err = compat_copy_struct(VAL_HW_LOCK_TYPE, COPY_TO_USER, (void *)data32, (void *)data);

            if (err)
            {
                return err;
            }
            return ret;
        }
        break;

        case VCODEC_INC_PWR_USER:
        case VCODEC_DEC_PWR_USER:
        {
            COMPAT_VAL_POWER_T __user *data32;
            VAL_POWER_T __user *data;
            int err;

            data32 = compat_ptr(arg);
            data = compat_alloc_user_space(sizeof(VAL_POWER_T));
            if (data == NULL)
            {
                return -EFAULT;
            }

            err = compat_copy_struct(VAL_POWER_TYPE, COPY_FROM_USER, (void *)data32, (void *)data);

            if (err)
            {
                return err;
            }

            ret = file->f_op->unlocked_ioctl(file, cmd, (unsigned long)data);

            err = compat_copy_struct(VAL_POWER_TYPE, COPY_TO_USER, (void *)data32, (void *)data);

            if (err)
            {
                return err;
            }
            return ret;
        }
        break;

        case VCODEC_WAITISR:
        {
            COMPAT_VAL_ISR_T __user *data32;
            VAL_ISR_T __user *data;
            int err;

            data32 = compat_ptr(arg);
            data = compat_alloc_user_space(sizeof(VAL_ISR_T));
            if (data == NULL)
            {
                return -EFAULT;
            }

            err = compat_copy_struct(VAL_ISR_TYPE, COPY_FROM_USER, (void *)data32, (void *)data);
            if (err)
            {
                return err;
            }

            ret = file->f_op->unlocked_ioctl(file, VCODEC_WAITISR, (unsigned long)data);

            err = compat_copy_struct(VAL_ISR_TYPE, COPY_TO_USER, (void *)data32, (void *)data);

            if (err)
            {
                return err;
            }
            return ret;
        }
        break;
        case VCODEC_INITHWLOCK:
        {
            COMPAT_VAL_VCODEC_OAL_HW_REGISTER_T __user *data32;
            VAL_VCODEC_OAL_HW_REGISTER_T __user *data;
            COMPAT_VAL_VCODEC_OAL_MEM_STAUTS_T __user *pHWStatus32;
            VAL_VCODEC_OAL_MEM_STAUTS_T __user *pHWStatus;
            VAL_UINT32_T    u4NumOfRegister;
            int err, i;

            data32 = compat_ptr(arg);
            data = compat_alloc_user_space(sizeof(VAL_VCODEC_OAL_HW_REGISTER_T) + sizeof(VAL_VCODEC_OAL_MEM_STAUTS_T) * 16);
            if (data == NULL)
            {
                return -EFAULT;
            }

            err = compat_copy_struct(VAL_VCODEC_OAL_HW_REGISTER_TYPE, COPY_FROM_USER, (void *)data32, (void *)data);

            pHWStatus32 = data->pHWStatus;
            u4NumOfRegister = data->u4NumOfRegister;
            pHWStatus = data + sizeof(VAL_VCODEC_OAL_HW_REGISTER_T);

            compat_uint_t u;
            compat_ulong_t l;
            for (i = 0; i < u4NumOfRegister; i++)
            {
                err |= get_user(l, &(pHWStatus32[i].u4ReadAddr));
                err |= put_user(l, &(pHWStatus[i].u4ReadAddr));
                err |= get_user(u, &(pHWStatus32[i].u4ReadData));
                err |= put_user(u, &(pHWStatus[i].u4ReadData));
                //MFV_LOGE("[%d] 0x%x, 0x%lx, %u, %u\n", i, l, pHWStatus[i].u4ReadAddr, u, pHWStatus[i].u4ReadData);
            }

            data->pHWStatus = pHWStatus;

            if (err)
            {
                return err;
            }

            mutex_lock(&InitHWLock);
            ori_user_data_addr = arg;
            ori_pHWStatus = pHWStatus32;
            ret = file->f_op->unlocked_ioctl(file, VCODEC_INITHWLOCK, (unsigned long)data);
            data->pHWStatus = pHWStatus32;
            ori_user_data_addr = 0;
            ori_pHWStatus = 0;
            mutex_unlock(&InitHWLock);

            err = compat_copy_struct(VAL_VCODEC_OAL_HW_REGISTER_TYPE, COPY_TO_USER, (void *)data32, (void *)data);

            if (err)
            {
                return err;
            }
            return ret;
        }
        break;
        default:
        {
            return vcodec_unlocked_ioctl(file, cmd, arg);
        }
        break;
    }
    return 0;
}
#else
#define vcodec_unlocked_compat_ioctl NULL
#endif
static int vcodec_open(struct inode *inode, struct file *file)
{
    MFV_LOGD("vcodec_open\n");

    mutex_lock(&DriverOpenCountLock);
    Driver_Open_Count++;

    MFV_LOGE("vcodec_open pid = %d, Driver_Open_Count %d\n", current->pid, Driver_Open_Count);
    mutex_unlock(&DriverOpenCountLock);


    // TODO: Check upper limit of concurrent users?

    return 0;
}

static int vcodec_flush(struct file *file, fl_owner_t id)
{
    MFV_LOGD("vcodec_flush, curr_tid =%d\n", current->pid);
    MFV_LOGE("vcodec_flush pid = %d, Driver_Open_Count %d\n", current->pid, Driver_Open_Count);

    return 0;
}

static int vcodec_release(struct inode *inode, struct file *file)
{
    VAL_INT32_T i, j;
    VAL_ULONG_T ulFlags, ulFlagsLockHW, ulFlagsISR, ulFlagsHWLock;

    //dump_stack();
    MFV_LOGD("vcodec_release, curr_tid =%d\n", current->pid);
    mutex_lock(&DriverOpenCountLock);
    MFV_LOGE("vcodec_release pid = %d, Driver_Open_Count %d\n", current->pid, Driver_Open_Count);
    Driver_Open_Count--;

    //mutex_lock(&NonCacheMemoryListLock);
    //Force_Free_NonCacheMemoryList(current->pid);
    //mutex_unlock(&NonCacheMemoryListLock);

    if (Driver_Open_Count == 0)
    {
        spin_lock_irqsave(&HWLock, ulFlagsHWLock);
        gu4VdecLockThreadId = 0;
        grVcodecHWLock.pvHandle = 0;
        grVcodecHWLock.eDriverType = VAL_DRIVER_TYPE_NONE;
        grVcodecHWLock.rLockedTime.u4Sec = 0;
        grVcodecHWLock.rLockedTime.u4uSec = 0;
        grVcodecHWLock.u4ThreadID = 0;
        grVcodecHWLock.u4VCodecThreadNum = VCODEC_THREAD_MAX_NUM;
        for (i = 0; i < VCODEC_THREAD_MAX_NUM; i++)
        {
            grVcodecHWLock.u4VCodecThreadID[i] = -1;
        }
        spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);

        mutex_lock(&DecEMILock);
        gu4DecEMICounter = 0;
        mutex_unlock(&DecEMILock);

        mutex_lock(&EncEMILock);
        gu4EncEMICounter = 0;
        mutex_unlock(&EncEMILock);

        mutex_lock(&PWRLock);
        gu4PWRCounter = 0;
        mutex_unlock(&PWRLock);

        mutex_lock(&NonCacheMemoryListLock);
        for (i = 0; i < VCODEC_MULTIPLE_INSTANCE_NUM_x_10; i++)
        {
            grNonCacheMemoryList[i].pvHandle = 0;
            for (j = 0; j < VCODEC_THREAD_MAX_NUM; j++)
            {
                grNonCacheMemoryList[i].u4VCodecThreadID[j] = 0xffffffff;
            }
            grNonCacheMemoryList[i].ulKVA = -1L;
            grNonCacheMemoryList[i].ulKPA = -1L;
        }
        mutex_unlock(&NonCacheMemoryListLock);

        spin_lock_irqsave(&OalHWContextLock, ulFlags);
        for (i = 0; i < VCODEC_MULTIPLE_INSTANCE_NUM; i++)
        {
            oal_hw_context[i].Oal_HW_reg = (VAL_VCODEC_OAL_HW_REGISTER_T *)0;
            oal_hw_context[i].ObjId = -1L;
            oal_hw_context[i].slotindex = i;
            oal_hw_context[i].u4VCodecThreadNum = VCODEC_THREAD_MAX_NUM;
            for (j = 0; j < VCODEC_THREAD_MAX_NUM; j++)
            {
                oal_hw_context[i].u4VCodecThreadID[j] = -1;
            }
            oal_hw_context[i].pvHandle = 0;
            oal_hw_context[i].u4NumOfRegister = 0;

            for (j = 0; j < OALMEM_STATUS_NUM ; j ++)
            {
                oal_hw_context[i].oalmem_status[j].u4ReadAddr = 0;
                oal_hw_context[i].oalmem_status[j].u4ReadData = 0;
            }

            // event part
            oal_hw_context[i].IsrEvent.pvHandle = "ISR_EVENT";
            oal_hw_context[i].IsrEvent.u4HandleSize = sizeof("ISR_EVENT") + 1;
            oal_hw_context[i].IsrEvent.u4TimeoutMs = 0xFFFFFFFF;  //1000;
        }
        spin_unlock_irqrestore(&OalHWContextLock, ulFlags);

#if defined(VENC_USE_L2C)
        mutex_lock(&L2CLock);
        if (gu4L2CCounter != 0)
        {
            MFV_LOGE("vcodec_flush pid = %d, L2 user = %d, force restore L2 settings\n", current->pid, gu4L2CCounter);
            if (config_L2(1))
            {
                MFV_LOGE("[VCODEC][ERROR] restore L2 settings failed\n");
            }
        }
        gu4L2CCounter = 0;
        mutex_unlock(&L2CLock);
#endif
        spin_lock_irqsave(&LockDecHWCountLock, ulFlagsLockHW);
        gu4LockDecHWCount = 0;
        spin_unlock_irqrestore(&LockDecHWCountLock, ulFlagsLockHW);

        spin_lock_irqsave(&LockEncHWCountLock, ulFlagsLockHW);
        gu4LockEncHWCount = 0;
        spin_unlock_irqrestore(&LockEncHWCountLock, ulFlagsLockHW);

        spin_lock_irqsave(&DecISRCountLock, ulFlagsISR);
        gu4DecISRCount = 0;
        spin_unlock_irqrestore(&DecISRCountLock, ulFlagsISR);

        spin_lock_irqsave(&EncISRCountLock, ulFlagsISR);
        gu4EncISRCount = 0;
        spin_unlock_irqrestore(&EncISRCountLock, ulFlagsISR);

#ifdef ENABLE_MMDVFS_VDEC
        if (VAL_TRUE == gMMDFVFSMonitorStarts)
        {
            gMMDFVFSMonitorStarts = VAL_FALSE;
            gMMDFVFSMonitorCounts = 0;
            gHWLockInterval = 0;
            gHWLockMaxDuration = 0;
            SendDvfsRequest(DVFS_LOW);
        }
#endif

    }

#ifdef ENABLE_MMDVFS_VDEC
    mutex_lock(&DecEMILock);
    if (VAL_TRUE == gMMDFVFSMonitorStarts && 0 == gu4DecEMICounter)
    {
        gMMDFVFSMonitorStarts = VAL_FALSE;
        gMMDFVFSMonitorCounts = 0;
        gHWLockInterval = 0;
        gHWLockMaxDuration = 0;
        SendDvfsRequest(DVFS_LOW);
    }
    mutex_unlock(&DecEMILock);
#endif

    mutex_unlock(&DriverOpenCountLock);

    return 0;
}

void vcodec_vma_open(struct vm_area_struct *vma)
{
    MFV_LOGD("vcodec VMA open, virt %lx, phys %lx\n", vma->vm_start, vma->vm_pgoff << PAGE_SHIFT);
}

void vcodec_vma_close(struct vm_area_struct *vma)
{
    MFV_LOGD("vcodec VMA close, virt %lx, phys %lx\n", vma->vm_start, vma->vm_pgoff << PAGE_SHIFT);
}

static struct vm_operations_struct vcodec_remap_vm_ops =
{
    .open = vcodec_vma_open,
    .close = vcodec_vma_close,
};

static int vcodec_mmap(struct file *file, struct vm_area_struct *vma)
{
#if 1
    VAL_UINT32_T u4I = 0;
    VAL_ULONG_T length;
    VAL_ULONG_T pfn;

    length = vma->vm_end - vma->vm_start;
    pfn = vma->vm_pgoff << PAGE_SHIFT;

    if (((length > VENC_REGION) || (pfn < VENC_BASE) || (pfn > VENC_BASE + VENC_REGION)) &&
        ((length > VDEC_REGION) || (pfn < VDEC_BASE_PHY) || (pfn > VDEC_BASE_PHY + VDEC_REGION)) &&
        ((length > HW_REGION) || (pfn < HW_BASE) || (pfn > HW_BASE + HW_REGION)) &&
        ((length > INFO_REGION) || (pfn < INFO_BASE) || (pfn > INFO_BASE + INFO_REGION)) &&
        ((length > MP4_VENC_REGION) || (pfn < MP4_VENC_BASE) || (pfn > MP4_VENC_BASE + MP4_VENC_REGION))
       )
    {
        VAL_ULONG_T ulAddr, ulSize;
        for (u4I = 0; u4I < VCODEC_MULTIPLE_INSTANCE_NUM_x_10; u4I++)
        {
            if ((grNonCacheMemoryList[u4I].ulKVA != -1L) && (grNonCacheMemoryList[u4I].ulKPA != -1L))
            {
                ulAddr = grNonCacheMemoryList[u4I].ulKPA;
                ulSize = (grNonCacheMemoryList[u4I].ulSize + 0x1000 - 1) & ~(0x1000 - 1);
                if ((length == ulSize) && (pfn == ulAddr))
                {
                    MFV_LOGD("[VCODEC] cache idx %d \n", u4I);
                    break;
                }
            }
        }

        if (u4I == VCODEC_MULTIPLE_INSTANCE_NUM_x_10)
        {
            MFV_LOGE("[VCODEC][ERROR] mmap region error: Length(0x%lx), pfn(0x%lx)\n", (VAL_ULONG_T)length, pfn);
            return -EAGAIN;
        }
    }
#endif
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    MFV_LOGE("[VCODEC][mmap] vma->start 0x%lx, vma->end 0x%lx, vma->pgoff 0x%lx, pfn: 0x%lx\n",
             (VAL_ULONG_T)vma->vm_start, (VAL_ULONG_T)vma->vm_end, (VAL_ULONG_T)vma->vm_pgoff, pfn);
    if (remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
                        vma->vm_end - vma->vm_start, vma->vm_page_prot))
    {
        return -EAGAIN;
    }

    vma->vm_ops = &vcodec_remap_vm_ops;
    vcodec_vma_open(vma);

    return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void vcodec_early_suspend(struct early_suspend *h)
{
    mutex_lock(&PWRLock);
    MFV_LOGE("vcodec_early_suspend, tid = %d, PWR_USER = %d\n", current->pid, gu4PWRCounter);
    mutex_unlock(&PWRLock);
    /*
        if (gu4PWRCounter != 0)
        {
            MFV_LOGE("[MT6589_VCodec_early_suspend] Someone Use HW, Disable Power!\n");
            disable_clock(MT65XX_PDN_MM_VBUF, "Video_VBUF");
            disable_clock(MT_CG_VDEC0_VDE, "VideoDec");
            disable_clock(MT_CG_VENC_VEN, "VideoEnc");
            disable_clock(MT65XX_PDN_MM_GDC_SHARE_MACRO, "VideoEnc");
        }
    */
    MFV_LOGD("vcodec_early_suspend - tid = %d\n", current->pid);
}

static void vcodec_late_resume(struct early_suspend *h)
{
    mutex_lock(&PWRLock);
    MFV_LOGE("vcodec_late_resume, tid = %d, PWR_USER = %d\n", current->pid, gu4PWRCounter);
    mutex_unlock(&PWRLock);
    /*
        if (gu4PWRCounter != 0)
        {
            MFV_LOGE("[vcodec_late_resume] Someone Use HW, Enable Power!\n");
            enable_clock(MT65XX_PDN_MM_VBUF, "Video_VBUF");
            enable_clock(MT_CG_VDEC0_VDE, "VideoDec");
            enable_clock(MT_CG_VENC_VEN, "VideoEnc");
            enable_clock(MT65XX_PDN_MM_GDC_SHARE_MACRO, "VideoEnc");
        }
    */
    MFV_LOGD("vcodec_late_resume - tid = %d\n", current->pid);
}

static struct early_suspend vcodec_early_suspend_handler =
{
    .level = (EARLY_SUSPEND_LEVEL_DISABLE_FB - 1),
    .suspend = vcodec_early_suspend,
    .resume = vcodec_late_resume,
};
#endif

static struct file_operations vcodec_fops =
{
    .owner      = THIS_MODULE,
    .unlocked_ioctl = vcodec_unlocked_ioctl,
    .open       = vcodec_open,
    .flush      = vcodec_flush,
    .release    = vcodec_release,
    .mmap       = vcodec_mmap,
#if IS_ENABLED(CONFIG_COMPAT)
    .compat_ioctl   = vcodec_unlocked_compat_ioctl,
#endif

};

static int vcodec_probe(struct platform_device *dev)
{
    int ret;
    MFV_LOGD("+vcodec_probe\n");

    mutex_lock(&DecEMILock);
    gu4DecEMICounter = 0;
    mutex_unlock(&DecEMILock);

    mutex_lock(&EncEMILock);
    gu4EncEMICounter = 0;
    mutex_unlock(&EncEMILock);

    mutex_lock(&PWRLock);
    gu4PWRCounter = 0;
    mutex_unlock(&PWRLock);

    mutex_lock(&L2CLock);
    gu4L2CCounter = 0;
    mutex_unlock(&L2CLock);

    ret = register_chrdev_region(vcodec_devno, 1, VCODEC_DEVNAME);
    if (ret)
    {
        MFV_LOGE("[ERROR] Can't Get Major number for VCodec Device\n");
    }

    vcodec_cdev = cdev_alloc();
    vcodec_cdev->owner = THIS_MODULE;
    vcodec_cdev->ops = &vcodec_fops;

    ret = cdev_add(vcodec_cdev, vcodec_devno, 1);
    if (ret)
    {
        MFV_LOGE("[ERROR] Can't add Vcodec Device\n");
    }

    vcodec_class = class_create(THIS_MODULE, VCODEC_DEVNAME);
    if (IS_ERR(vcodec_class))
    {
        ret = PTR_ERR(vcodec_class);
        MFV_LOGE("[VCODEC][ERROR] Unable to create class, err = %d", ret);
        return ret;
    }

    vcodec_device = device_create(vcodec_class, NULL, vcodec_devno, NULL, VCODEC_DEVNAME);

    //if (request_irq(MT_VDEC_IRQ_ID , (irq_handler_t)video_intr_dlr, IRQF_TRIGGER_LOW, VCODEC_DEVNAME, NULL) < 0)
    if (request_irq(VDEC_IRQ_ID , (irq_handler_t)video_intr_dlr, IRQF_TRIGGER_LOW, VCODEC_DEVNAME, NULL) < 0)
    {
        MFV_LOGE("[VCODEC][ERROR] error to request dec irq\n");
    }
    else
    {
        MFV_LOGD("[VCODEC] success to request dec irq: %d\n", VDEC_IRQ_ID);
    }

    //if (request_irq(MT_VENC_IRQ_ID , (irq_handler_t)video_intr_dlr2, IRQF_TRIGGER_LOW, VCODEC_DEVNAME, NULL) < 0)
    if (request_irq(VENC_IRQ_ID , (irq_handler_t)video_intr_dlr2, IRQF_TRIGGER_LOW, VCODEC_DEVNAME, NULL) < 0)
    {
        MFV_LOGD("[VCODEC][ERROR] error to request enc irq\n");
    }
    else
    {
        MFV_LOGD("[VCODEC] success to request enc irq: %d\n", VENC_IRQ_ID);
    }

    //disable_irq(MT_VDEC_IRQ_ID);
    disable_irq(VDEC_IRQ_ID);
    //disable_irq(MT_VENC_IRQ_ID);
    disable_irq(VENC_IRQ_ID);

    vcodec_device->coherent_dma_mask = DMA_BIT_MASK(32);
    if (!vcodec_device->dma_mask)
    {
        vcodec_device->dma_mask = &vcodec_device->coherent_dma_mask;
    }

    MFV_LOGD("vcodec_probe Done\n");

    return 0;
}

#ifdef CONFIG_MTK_HIBERNATION
extern void mt_irq_set_sens(unsigned int irq, unsigned int sens);
extern void mt_irq_set_polarity(unsigned int irq, unsigned int polarity);
static int vcodec_pm_restore_noirq(struct device *device)
{
    // vdec : IRQF_TRIGGER_LOW
    mt_irq_set_sens(VDEC_IRQ_ID, MT_LEVEL_SENSITIVE);
    mt_irq_set_polarity(VDEC_IRQ_ID, MT_POLARITY_LOW);
    // venc: IRQF_TRIGGER_LOW
    mt_irq_set_sens(VENC_IRQ_ID, MT_LEVEL_SENSITIVE);
    mt_irq_set_polarity(VENC_IRQ_ID, MT_POLARITY_LOW);

    return 0;
}
#endif

static int __init vcodec_driver_init(void)
{
    int i, j;
    VAL_RESULT_T  eValHWLockRet, eValRet;
    VAL_ULONG_T ulFlags, ulFlagsLockHW, ulFlagsISR, ulFlagsHWLock;
    VAL_ULONG_T ulFlagsVdecPWRLock, ulFlagsVencPWRLock;

    MFV_LOGD("+vcodec_driver_init !!\n");

    mutex_lock(&DriverOpenCountLock);
    Driver_Open_Count = 0;
    mutex_unlock(&DriverOpenCountLock);

    {
#ifdef CONFIG_ARCH_MT6580
        struct device_node *node = NULL;
        node = of_find_compatible_node(NULL, NULL, "mediatek,VENC");
        if (node)
        {
            KVA_VENC_BASE = (VAL_ULONG_T)of_iomap(node, 0);
            VENC_IRQ_ID =  irq_of_parse_and_map(node, 0);
        }
        else
        {
            MFV_LOGD("[VCODEC][DeviceTree] Not supported, use hard-code value")
            KVA_VENC_BASE  = (VAL_ULONG_T)ioremap(0x14016000, 0x800);
            //VENC_IRQ_ID = MT_VENC_IRQ_ID;
        }
        KVA_VENC_IRQ_STATUS_ADDR =    KVA_VENC_BASE + 0x67C;
        KVA_VENC_IRQ_ACK_ADDR  = KVA_VENC_BASE + 0x678;
        KVA_VENC_ZERO_COEF_COUNT_ADDR = KVA_VENC_BASE + 0x688;
        KVA_VENC_BYTE_COUNT_ADDR = KVA_VENC_BASE + 0x680;
        KVA_VENC_MP4_IRQ_ENABLE_ADDR = KVA_VENC_BASE + 0x668;
#else
        struct device_node *node = NULL;
        node = of_find_compatible_node(NULL, NULL, "mediatek,VENC");
        KVA_VENC_BASE = (VAL_ULONG_T)of_iomap(node, 0);
        VENC_IRQ_ID =  irq_of_parse_and_map(node, 0);
        KVA_VENC_IRQ_STATUS_ADDR =    KVA_VENC_BASE + 0x05C;
        KVA_VENC_IRQ_ACK_ADDR  = KVA_VENC_BASE + 0x060;
#endif
    }
#ifdef CONFIG_ARCH_MT6580
    {
        struct device_node *node = NULL;
        node = of_find_compatible_node(NULL, NULL, "mediatek,VDEC");
        if (node)
        {
            KVA_VDEC_BASE = (VAL_ULONG_T)of_iomap(node, 0);
            VDEC_IRQ_ID =  irq_of_parse_and_map(node, 0);
        }
        else
        {
            MFV_LOGE("[VCODEC][DeviceTree] VDEC Not supported, use hard-code value")
            //KVA_VDEC_BASE  = (VAL_ULONG_T)ioremap(0x1500B000, 0x1000);
            KVA_VDEC_BASE  = (VAL_ULONG_T)ioremap(VDEC_BASE_PHY, VDEC_REGION);
            VDEC_IRQ_ID =  137;//0x6c; //rainier Hybrid VDEC interrupt ID is 0x6c
        }
        KVA_VDEC_MISC_BASE = KVA_VDEC_BASE + 0x0000;
        KVA_VDEC_VLD_BASE = KVA_VDEC_BASE + 0x1000;
        KVA_VDEC_INT_STA_ADDR = KVA_VDEC_BASE + 0x10; // VDEC interrupt status
        KVA_VDEC_INT_ACK_ADDR = KVA_VDEC_BASE + 0xc;  // VDEC interrupt ACKnowledge
    }

    {
        //struct device_node *node = NULL;
        //node = of_find_compatible_node(NULL, NULL, "mediatek,VDEC_GCON");
        //KVA_VDEC_GCON_BASE = (VAL_ULONG_T)of_iomap(node, 0);

        MFV_LOGE("[VCODEC][DeviceTree] KVA_VENC_BASE(0x%lx), KVA_VDEC_BASE(0x%lx), KVA_VDEC_GCON_BASE(0x%lx)", KVA_VENC_BASE, KVA_VDEC_BASE, KVA_VDEC_GCON_BASE);
        MFV_LOGD("[VCODEC][DeviceTree] VDEC_IRQ_ID(%d), VENC_IRQ_ID(%d)", VDEC_IRQ_ID, VENC_IRQ_ID);
    }
#else
    {
        struct device_node *node = NULL;
        node = of_find_compatible_node(NULL, NULL, "mediatek,VDEC_FULL_TOP");
        KVA_VDEC_BASE = (VAL_ULONG_T)of_iomap(node, 0);
        VDEC_IRQ_ID =  irq_of_parse_and_map(node, 0);
        KVA_VDEC_MISC_BASE = KVA_VDEC_BASE + 0x0000;
        KVA_VDEC_VLD_BASE = KVA_VDEC_BASE + 0x1000;
    }
    {
        struct device_node *node = NULL;
        node = of_find_compatible_node(NULL, NULL, "mediatek,VDEC_GCON");
        KVA_VDEC_GCON_BASE = (VAL_ULONG_T)of_iomap(node, 0);

        MFV_LOGD("[VCODEC][DeviceTree] KVA_VENC_BASE(0x%lx), KVA_VDEC_BASE(0x%lx), KVA_VDEC_GCON_BASE(0x%lx)", KVA_VENC_BASE, KVA_VDEC_BASE, KVA_VDEC_GCON_BASE);
        MFV_LOGD("[VCODEC][DeviceTree] VDEC_IRQ_ID(%d), VENC_IRQ_ID(%d)", VDEC_IRQ_ID, VENC_IRQ_ID);
    }
#endif
    //    KVA_VENC_IRQ_STATUS_ADDR =    (VAL_ULONG_T)ioremap(VENC_IRQ_STATUS_addr, 4);
    //    KVA_VENC_IRQ_ACK_ADDR  = (VAL_ULONG_T)ioremap(VENC_IRQ_ACK_addr, 4);


#ifdef VENC_PWR_FPGA    // useless 2014_3_4
    KVA_VENC_CLK_CFG_0_ADDR = (VAL_ULONG_T)ioremap(CLK_CFG_0_addr, 4);
    KVA_VENC_CLK_CFG_4_ADDR = (VAL_ULONG_T)ioremap(CLK_CFG_4_addr, 4);
    KVA_VENC_PWR_ADDR = (VAL_ULONG_T)ioremap(VENC_PWR_addr, 4);
    KVA_VENCSYS_CG_SET_ADDR = (VAL_ULONG_T)ioremap(VENCSYS_CG_SET_addr, 4);
#endif

    gu4VDecIRQCount = 0;
    gu4VEncIRQCount = 0;

    spin_lock_irqsave(&LockDecHWCountLock, ulFlagsLockHW);
    gu4LockDecHWCount = 0;
    spin_unlock_irqrestore(&LockDecHWCountLock, ulFlagsLockHW);

    spin_lock_irqsave(&LockEncHWCountLock, ulFlagsLockHW);
    gu4LockEncHWCount = 0;
    spin_unlock_irqrestore(&LockEncHWCountLock, ulFlagsLockHW);

    spin_lock_irqsave(&DecISRCountLock, ulFlagsISR);
    gu4DecISRCount = 0;
    spin_unlock_irqrestore(&DecISRCountLock, ulFlagsISR);

    spin_lock_irqsave(&EncISRCountLock, ulFlagsISR);
    gu4EncISRCount = 0;
    spin_unlock_irqrestore(&EncISRCountLock, ulFlagsISR);

    spin_lock_irqsave(&VdecPWRLock, ulFlagsVdecPWRLock);
    gu4VdecPWRCounter = 0;
    spin_unlock_irqrestore(&VdecPWRLock, ulFlagsVdecPWRLock);

    spin_lock_irqsave(&VencPWRLock, ulFlagsVencPWRLock);
    gu4VencPWRCounter = 0;
    spin_unlock_irqrestore(&VencPWRLock, ulFlagsVencPWRLock);

    mutex_lock(&IsOpenedLock);
    if (VAL_FALSE == bIsOpened)
    {
        bIsOpened = VAL_TRUE;
        vcodec_probe(NULL);
    }
    mutex_unlock(&IsOpenedLock);

    spin_lock_irqsave(&HWLock, ulFlagsHWLock);
    gu4VdecLockThreadId = 0;
    grVcodecHWLock.pvHandle = 0;
    grVcodecHWLock.eDriverType = VAL_DRIVER_TYPE_NONE;
    grVcodecHWLock.rLockedTime.u4Sec = 0;
    grVcodecHWLock.rLockedTime.u4uSec = 0;
    grVcodecHWLock.u4ThreadID = 0;
    grVcodecHWLock.u4VCodecThreadNum = VCODEC_THREAD_MAX_NUM;
    for (i = 0; i < VCODEC_THREAD_MAX_NUM; i++)
    {
        grVcodecHWLock.u4VCodecThreadID[i] = -1;
    }
    spin_unlock_irqrestore(&HWLock, ulFlagsHWLock);

    mutex_lock(&NonCacheMemoryListLock);
    for (i = 0; i < VCODEC_MULTIPLE_INSTANCE_NUM_x_10; i++)
    {
        grNonCacheMemoryList[i].pvHandle = 0x0;
        for (j = 0; j < VCODEC_THREAD_MAX_NUM; j++)
        {
            grNonCacheMemoryList[i].u4VCodecThreadID[j] = 0xffffffff;
        }
        grNonCacheMemoryList[i].ulKVA = -1L;
        grNonCacheMemoryList[i].ulKPA = -1L;
    }
    mutex_unlock(&NonCacheMemoryListLock);

    for (i = 0; i < VCODEC_MULTIPLE_INSTANCE_NUM; i++)
    {
        spin_lock_irqsave(&OalHWContextLock, ulFlags);
        oal_hw_context[i].Oal_HW_reg = (VAL_VCODEC_OAL_HW_REGISTER_T *)0;
        oal_hw_context[i].ObjId = -1L;
        oal_hw_context[i].slotindex = i;
        oal_hw_context[i].u4VCodecThreadNum = VCODEC_THREAD_MAX_NUM;
        for (j = 0; j < VCODEC_THREAD_MAX_NUM; j++)
        {
            oal_hw_context[i].u4VCodecThreadID[j] = -1;
        }
        oal_hw_context[i].pvHandle = 0;
        oal_hw_context[i].u4NumOfRegister = 0;

        for (j = 0; j < OALMEM_STATUS_NUM ; j ++)
        {
            oal_hw_context[i].oalmem_status[j].u4ReadAddr = 0;
            oal_hw_context[i].oalmem_status[j].u4ReadData = 0;
        }

        // event part
        oal_hw_context[i].IsrEvent.pvHandle = "ISR_EVENT";
        oal_hw_context[i].IsrEvent.u4HandleSize = sizeof("ISR_EVENT") + 1;
        oal_hw_context[i].IsrEvent.u4TimeoutMs = 0xFFFFFFFF;  //1000;
        spin_unlock_irqrestore(&OalHWContextLock, ulFlags);
        eValRet = eVideoCreateEvent(&(oal_hw_context[i].IsrEvent), sizeof(VAL_EVENT_T));
        if (VAL_RESULT_NO_ERROR != eValRet)
        {
            MFV_LOGE("[MFV][ERROR] create isr event error\n");
        }
    }

    //HWLockEvent part
    mutex_lock(&HWLockEventTimeoutLock);
    HWLockEvent.pvHandle = "HWLOCK_EVENT";
    HWLockEvent.u4HandleSize = sizeof("HWLOCK_EVENT") + 1;
    HWLockEvent.u4TimeoutMs = 1;
    mutex_unlock(&HWLockEventTimeoutLock);
    eValHWLockRet = eVideoCreateEvent(&HWLockEvent, sizeof(VAL_EVENT_T));
    if (VAL_RESULT_NO_ERROR != eValHWLockRet)
    {
        MFV_LOGE("[VCODEC][ERROR] create hwlock event error\n");
    }

    //IsrEvent part
    spin_lock_irqsave(&DecIsrLock, ulFlags);
    DecIsrEvent.pvHandle = "DECISR_EVENT";
    DecIsrEvent.u4HandleSize = sizeof("DECISR_EVENT") + 1;
    DecIsrEvent.u4TimeoutMs = 1;
    spin_unlock_irqrestore(&DecIsrLock, ulFlags);
    eValHWLockRet = eVideoCreateEvent(&DecIsrEvent, sizeof(VAL_EVENT_T));
    if (VAL_RESULT_NO_ERROR != eValHWLockRet)
    {
        MFV_LOGE("[VCODEC][ERROR] create dec isr event error\n");
    }

    spin_lock_irqsave(&EncIsrLock, ulFlags);
    EncIsrEvent.pvHandle = "ENCISR_EVENT";
    EncIsrEvent.u4HandleSize = sizeof("ENCISR_EVENT") + 1;
    EncIsrEvent.u4TimeoutMs = 1;
    spin_unlock_irqrestore(&EncIsrLock, ulFlags);
    eValHWLockRet = eVideoCreateEvent(&EncIsrEvent, sizeof(VAL_EVENT_T));
    if (VAL_RESULT_NO_ERROR != eValHWLockRet)
    {
        MFV_LOGE("[VCODEC][ERROR] create enc isr event error\n");
    }

    m4u_register_fault_callback(M4U_PORT_VENC_CDMA_VDEC_CDMA, vcodec_m4u_fault_callback, (void*)&grVcodecHWLock);
    m4u_register_fault_callback(M4U_PORT_VENC_BSDMA_VDEC_POST0, vcodec_m4u_fault_callback, (void*)&grVcodecHWLock);
    MFV_LOGD("vcodec_driver_init Done\n");

#ifdef CONFIG_HAS_EARLYSUSPEND
    register_early_suspend(&vcodec_early_suspend_handler);
#endif

#ifdef CONFIG_MTK_HIBERNATION
    register_swsusp_restore_noirq_func(ID_M_VCODEC, vcodec_pm_restore_noirq, NULL);
#endif

    return 0;
}

static void __exit vcodec_driver_exit(void)
{
    int  i;
    VAL_RESULT_T  eValRet;
    VAL_RESULT_T  eValHWLockRet;

    MFV_LOGD("vcodec_driver_exit\n");

    mutex_lock(&IsOpenedLock);
    if (VAL_TRUE == bIsOpened)
    {
        bIsOpened = VAL_FALSE;
    }
    mutex_unlock(&IsOpenedLock);

    cdev_del(vcodec_cdev);
    unregister_chrdev_region(vcodec_devno, 1);

    // [TODO] iounmap the following?
#if 0
    iounmap((void *)KVA_VENC_IRQ_STATUS_ADDR);
    iounmap((void *)KVA_VENC_IRQ_ACK_ADDR);
#endif
#ifdef VENC_PWR_FPGA
    iounmap((void *)KVA_VENC_CLK_CFG_0_ADDR);
    iounmap((void *)KVA_VENC_CLK_CFG_4_ADDR);
    iounmap((void *)KVA_VENC_PWR_ADDR);
    iounmap((void *)KVA_VENCSYS_CG_SET_ADDR);
#else
    // TODO:
    //iounmap((void *)KVA_VENC_BASE);
#endif

    // [TODO] free IRQ here
    //free_irq(MT_VENC_IRQ_ID, NULL);
    free_irq(VENC_IRQ_ID, NULL);
    //free_irq(MT_VDEC_IRQ_ID, NULL);
    free_irq(VDEC_IRQ_ID, NULL);

    for (i = 0; i < OALMEM_STATUS_NUM ; i++)
    {
        eValRet = eVideoCloseEvent(&(oal_hw_context[i].IsrEvent), sizeof(VAL_EVENT_T));
        if (VAL_RESULT_NO_ERROR != eValRet)
        {
            MFV_LOGE("[VCODEC][ERROR] close isr event error\n");
        }
    }

    eValHWLockRet = eVideoCloseEvent(&HWLockEvent, sizeof(VAL_EVENT_T));
    if (VAL_RESULT_NO_ERROR != eValHWLockRet)
    {
        MFV_LOGE("[VCODEC][ERROR] close hwlock event error\n");
    }

    eValHWLockRet = eVideoCloseEvent(&DecIsrEvent, sizeof(VAL_EVENT_T));
    if (VAL_RESULT_NO_ERROR != eValHWLockRet)
    {
        MFV_LOGE("[VCODEC][ERROR] close dec isr event error\n");
    }

    eValHWLockRet = eVideoCloseEvent(&EncIsrEvent, sizeof(VAL_EVENT_T));
    if (VAL_RESULT_NO_ERROR != eValHWLockRet)
    {
        MFV_LOGE("[VCODEC][ERROR] close enc isr event error\n");
    }

    m4u_unregister_fault_callback(M4U_PORT_VENC_CDMA_VDEC_CDMA);
    m4u_unregister_fault_callback(M4U_PORT_VENC_BSDMA_VDEC_POST0);

#ifdef CONFIG_HAS_EARLYSUSPEND
    unregister_early_suspend(&vcodec_early_suspend_handler);
#endif

#ifdef CONFIG_MTK_HIBERNATION
    unregister_swsusp_restore_noirq_func(ID_M_VCODEC);
#endif
}

module_init(vcodec_driver_init);
module_exit(vcodec_driver_exit);
MODULE_AUTHOR("Legis, Lu <legis.lu@mediatek.com>");
MODULE_DESCRIPTION("Rainier Vcodec Driver");
MODULE_LICENSE("GPL");

