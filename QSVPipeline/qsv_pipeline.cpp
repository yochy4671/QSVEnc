﻿// -----------------------------------------------------------------------------------------
// QSVEnc by rigaya
// -----------------------------------------------------------------------------------------
// The MIT License
//
// Copyright (c) 2011-2016 rigaya
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// ------------------------------------------------------------------------------------------

#include "rgy_tchar.h"
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <sstream>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <numeric>
#include <cassert>
#include <climits>
#include <deque>
#include <mutex>
#pragma warning(push)
#pragma warning(disable: 4244)
#pragma warning(disable: 4834)
#define TTMATH_NOASM
#include "ttmath/ttmath.h"
#pragma warning(pop)
#include "rgy_osdep.h"
#include "qsv_pipeline.h"
#include "qsv_pipeline_ctrl.h"
#include "qsv_query.h"
#include "rgy_def.h"
#include "rgy_env.h"
#include "rgy_filesystem.h"
#include "rgy_input.h"
#include "rgy_output.h"
#include "rgy_input_raw.h"
#include "rgy_input_vpy.h"
#include "rgy_input_avs.h"
#include "rgy_input_avi.h"
#include "rgy_input_sm.h"
#include "rgy_input_avcodec.h"
#include "rgy_filter.h"
#include "rgy_filter_colorspace.h"
#include "rgy_filter_afs.h"
#include "rgy_filter_nnedi.h"
#include "rgy_filter_mpdecimate.h"
#include "rgy_filter_decimate.h"
#include "rgy_filter_delogo.h"
#include "rgy_filter_denoise_knn.h"
#include "rgy_filter_denoise_pmd.h"
#include "rgy_filter_smooth.h"
#include "rgy_filter_subburn.h"
#include "rgy_filter_transform.h"
#include "rgy_filter_unsharp.h"
#include "rgy_filter_edgelevel.h"
#include "rgy_filter_warpsharp.h"
#include "rgy_filter_deband.h"
#include "rgy_filter_ssim.h"
#include "rgy_filter_tweak.h"
#include "rgy_output_avcodec.h"
#include "rgy_bitstream.h"
#include "qsv_hw_device.h"
#include "qsv_allocator.h"
#include "qsv_allocator_sys.h"
#include "rgy_avlog.h"
#include "rgy_chapter.h"
#include "rgy_timecode.h"
#include "rgy_aspect_ratio.h"
#include "rgy_codepage.h"
#if defined(_WIN32) || defined(_WIN64)
#include "api_hook.h"
#endif

#if D3D_SURFACES_SUPPORT
#include "qsv_hw_d3d9.h"
#include "qsv_hw_d3d11.h"

#include "qsv_allocator_d3d9.h"
#include "qsv_allocator_d3d11.h"
#endif

#if LIBVA_SUPPORT
#include "qsv_hw_va.h"
#include "qsv_allocator_va.h"
#endif

RGY_ERR CreateAllocatorImpl(
    std::unique_ptr<QSVAllocator>& allocator, std::unique_ptr<mfxAllocatorParams>& allocParams, bool& externalAlloc,
    const MemType memType, CQSVHWDevice *hwdev, MFXVideoSession& session, std::shared_ptr<RGYLog>& log) {
    auto sts = RGY_ERR_NONE;
    if (log) log->write(RGY_LOG_DEBUG, _T("CreateAllocator: MemType: %s\n"), MemTypeToStr(memType));

#define CA_ERR(ret, MES)    {if (RGY_ERR_NONE > (ret)) { if (log) log->write(RGY_LOG_ERROR, _T("%s : %s\n"), MES, get_err_mes(ret)); return ret;}}

    if (D3D9_MEMORY == memType || D3D11_MEMORY == memType || VA_MEMORY == memType || HW_MEMORY == memType) {
#if D3D_SURFACES_SUPPORT
        const mfxHandleType hdl_t = mfxHandleTypeFromMemType(memType, false);
        mfxHDL hdl = NULL;
        sts = err_to_rgy(hwdev->GetHandle(hdl_t, &hdl));
        CA_ERR(sts, _T("Failed to get HW device handle."));
        if (log) log->write(RGY_LOG_DEBUG, _T("CreateAllocator: HW device GetHandle success.\n"));

        mfxIMPL impl = 0;
        session.QueryIMPL(&impl);
        if (impl != MFX_IMPL_SOFTWARE) {
            // hwエンコード時のみハンドルを渡す
            sts = err_to_rgy(session.SetHandle(hdl_t, hdl));
            CA_ERR(sts, _T("Failed to set HW device handle to encode session."));
            if (log) log->write(RGY_LOG_DEBUG, _T("CreateAllocator: set HW device handle to encode session.\n"));
        }

        //D3D allocatorを作成
#if MFX_D3D11_SUPPORT
        if (D3D11_MEMORY == memType) {
            if (log) log->write(RGY_LOG_DEBUG, _T("CreateAllocator: Create d3d11 allocator.\n"));
            allocator.reset(new QSVAllocatorD3D11);
            if (!allocator) {
                if (log) log->write(RGY_LOG_ERROR, _T("Failed to allcate memory for D3D11FrameAllocator.\n"));
                return RGY_ERR_MEMORY_ALLOC;
            }

            QSVAllocatorParamsD3D11 *pd3dAllocParams = new QSVAllocatorParamsD3D11;
            if (!pd3dAllocParams) {
                if (log) log->write(RGY_LOG_ERROR, _T("Failed to allcate memory for D3D11AllocatorParams.\n"));
                return RGY_ERR_MEMORY_ALLOC;
            }
            pd3dAllocParams->pDevice = reinterpret_cast<ID3D11Device *>(hdl);
            if (log) log->write(RGY_LOG_DEBUG, _T("CreateAllocator: d3d11...\n"));

            allocParams.reset(pd3dAllocParams);
        } else
#endif // #if MFX_D3D11_SUPPORT
        {
            if (log) log->write(RGY_LOG_DEBUG, _T("CreateAllocator: Create d3d9 allocator.\n"));
            allocator.reset(new QSVAllocatorD3D9);
            if (!allocator) {
                if (log) log->write(RGY_LOG_ERROR, _T("Failed to allcate memory for D3DFrameAllocator.\n"));
                return RGY_ERR_MEMORY_ALLOC;
            }

            QSVAllocatorParamsD3D9 *pd3dAllocParams = new QSVAllocatorParamsD3D9;
            if (!pd3dAllocParams) {
                if (log) log->write(RGY_LOG_ERROR, _T("Failed to allcate memory for pd3dAllocParams.\n"));
                return RGY_ERR_MEMORY_ALLOC;
            }
            pd3dAllocParams->pManager = reinterpret_cast<IDirect3DDeviceManager9 *>(hdl);
            //通常、OpenCL-d3d9間のinteropでrelease/acquireで余計なオーバーヘッドが発生させないために、
            //shared_handleを取得する必要がある(qsv_opencl.hのgetOpenCLFrameInterop()参照)
            //shared_handleはd3d9でCreateSurfaceする際に取得する。
            //しかし、これを取得しようとするとWin7のSandybridge環境ではデコードが正常に行われなくなってしまう問題があるとの報告を受けた
            //そのため、shared_handleを取得するのは、SandyBridgeでない環境に限るようにする
            pd3dAllocParams->getSharedHandle = getCPUGen(&session) != CPU_GEN_SANDYBRIDGE;
            if (log) log->write(RGY_LOG_DEBUG, _T("CreateAllocator: d3d9 (getSharedHandle = %s)...\n"), pd3dAllocParams->getSharedHandle ? _T("true") : _T("false"));

            allocParams.reset(pd3dAllocParams);
        }

        //GPUメモリ使用時には external allocatorを使用する必要がある
        //mfxSessionにallocatorを渡してやる必要がある
        sts = err_to_rgy(session.SetFrameAllocator(allocator.get()));
        CA_ERR(sts, _T("Failed to set frame allocator to encode session."));
        if (log) log->write(RGY_LOG_DEBUG, _T("CreateAllocator: frame allocator set to session.\n"));

        externalAlloc = true;
#endif
#if LIBVA_SUPPORT
        sts = CreateHWDevice();
        CA_ERR(sts, _T("Failed to CreateHWDevice."));

        mfxHDL hdl = NULL;
        sts = err_to_rgy(hwdev->GetHandle(MFX_HANDLE_VA_DISPLAY, &hdl));
        CA_ERR(sts, _T("Failed to get HW device handle."));
        if (log) log->write(RGY_LOG_DEBUG, _T("CreateAllocator: HW device GetHandle success. : 0x%x\n"), (uint32_t)(size_t)hdl);

        //ハンドルを渡す
        sts = err_to_rgy(session.SetHandle(MFX_HANDLE_VA_DISPLAY, hdl));
        CA_ERR(sts, _T("Failed to set HW device handle to encode session."));

        //VAAPI allocatorを作成
        allocator.reset(new QSVAllocatorVA());
        if (!allocator) {
            if (log) log->write(RGY_LOG_ERROR, _T("Failed to allcate memory for vaapiFrameAllocator.\n"));
            return RGY_ERR_MEMORY_ALLOC;
        }

        QSVAllocatorParamsVA *p_vaapiAllocParams = new QSVAllocatorParamsVA();
        if (!p_vaapiAllocParams) {
            if (log) log->write(RGY_LOG_ERROR, _T("Failed to allcate memory for vaapiAllocatorParams.\n"));
            return RGY_ERR_MEMORY_ALLOC;
        }

        p_vaapiAllocParams->m_dpy = (VADisplay)hdl;
        allocParams.reset(p_vaapiAllocParams);

        //GPUメモリ使用時には external allocatorを使用する必要がある
        //mfxSessionにallocatorを渡してやる必要がある
        sts = err_to_rgy(session.SetFrameAllocator(allocator.get()));
        CA_ERR(sts, _T("Failed to set frame allocator to encode session."));
        if (log) log->write(RGY_LOG_DEBUG, _T("CreateAllocator: frame allocator set to session.\n"));

        externalAlloc = true;
#endif
    } else {
#if LIBVA_SUPPORT
        //システムメモリ使用でも MFX_HANDLE_VA_DISPLAYをHW libraryに渡してやる必要がある
        mfxIMPL impl;
        session.QueryIMPL(&impl);

        if (MFX_IMPL_HARDWARE == MFX_IMPL_BASETYPE(impl)) {
            sts = CreateHWDevice();
            CA_ERR(sts, _T("Failed to CreateHWDevice."));

            mfxHDL hdl = NULL;
            sts = err_to_rgy(hwdev->GetHandle(MFX_HANDLE_VA_DISPLAY, &hdl));
            CA_ERR(sts, _T("Failed to get HW device handle."));
            if (log) log->write(RGY_LOG_DEBUG, _T("CreateAllocator: HW device GetHandle success. : 0x%x\n"), (uint32_t)(size_t)hdl);

            //ハンドルを渡す
            sts = err_to_rgy(session.SetHandle(MFX_HANDLE_VA_DISPLAY, hdl));
            CA_ERR(sts, _T("Failed to set HW device handle to encode session."));
        }
#endif
        //system memory allocatorを作成
        allocator.reset(new QSVAllocatorSys);
        if (!allocator) {
            return RGY_ERR_MEMORY_ALLOC;
        }
        if (log) log->write(RGY_LOG_DEBUG, _T("CreateAllocator: sys mem allocator...\n"));
    }

    //メモリallocatorの初期化
    if (RGY_ERR_NONE > (sts = err_to_rgy(allocator->Init(allocParams.get(), log)))) {
        if (log) log->write(RGY_LOG_ERROR, _T("Failed to initialize %s memory allocator. : %s\n"), MemTypeToStr(memType), get_err_mes(sts));
        return sts;
    }
    if (log) log->write(RGY_LOG_DEBUG, _T("CreateAllocator: frame allocator initialized.\n"));
#undef CA_ERR
    return RGY_ERR_NONE;
}

#define RGY_ERR_MES(ret, MES)    {if (RGY_ERR_NONE > (ret)) { PrintMes(RGY_LOG_ERROR, _T("%s : %s\n"), MES, get_err_mes(ret)); return err_to_mfx(ret);}}
#define RGY_ERR(ret, MES)    {if (RGY_ERR_NONE > (ret)) { PrintMes(RGY_LOG_ERROR, _T("%s : %s\n"), MES, get_err_mes(ret)); return ret;}}
#define QSV_ERR_MES(sts, MES)    {if (MFX_ERR_NONE > (sts)) { PrintMes(RGY_LOG_ERROR, _T("%s : %s\n"), MES, get_err_mes((int)sts)); return sts;}}
#define CHECK_RANGE_LIST(value, list, name)    { if (CheckParamList((value), (list), (name)) != RGY_ERR_NONE) { return RGY_ERR_INVALID_VIDEO_PARAM; } }

int CQSVPipeline::clamp_param_int(int value, int low, int high, const TCHAR *param_name) {
    auto value_old = value;
    value = clamp(value, low, high);
    if (value != value_old) {
        PrintMes(RGY_LOG_WARN, _T("%s value changed %d -> %d, must be in range of %d-%d\n"), param_name, value_old, value, low, high);
    }
    return value;
}

bool CQSVPipeline::CompareParam(const mfxParamSet& prmIn, const mfxParamSet& prmOut) {
    bool ret = false;
#define COMPARE_INT(member, ignoreIfInput) { \
    if (prmIn.member != prmOut.member) { \
        ret = true;\
        PrintMes(((int64_t)prmIn.member == (int64_t)ignoreIfInput) ? RGY_LOG_DEBUG : RGY_LOG_WARN, _T("%s value changed %d -> %d by driver\n"), _T(#member), (int)prmIn.member, (int)prmOut.member); \
    }}
#define TRI_STATE(x) ((x == 0) ? _T("auto") : ((x == MFX_CODINGOPTION_ON) ? _T("on") : _T("off")))
#define COMPARE_TRI(member, ignoreIfInput) { \
    if (prmIn.member != prmOut.member) { \
        ret = true;\
        PrintMes((prmIn.member == ignoreIfInput) ? RGY_LOG_DEBUG : RGY_LOG_WARN, _T("%s value changed %s -> %s by driver\n"), _T(#member), TRI_STATE(prmIn.member), TRI_STATE(prmOut.member)); \
    }}
#define COMPARE_HEX(member, ignoreIfInput) { \
    if (prmIn.member != prmOut.member) { \
        ret = true;\
        PrintMes((prmIn.member == ignoreIfInput) ? RGY_LOG_DEBUG : RGY_LOG_WARN, _T("%s value changed 0x%x -> 0x%x by driver\n"), _T(#member), (int)prmIn.member, (int)prmOut.member); \
    }}
#define COMPARE_DBL(member, ignoreIfInput) { \
    if (prmIn.member != prmOut.member) { \
        ret = true;\
        PrintMes((prmIn.member == ignoreIfInput) ? RGY_LOG_DEBUG : RGY_LOG_WARN, _T("%s value changed %lf -> %lf by driver\n"), _T(#member), (double)prmIn.member, (double)prmOut.member); \
    }}
#define COMPARE_STR(member, ignoreIfInput, printMethod) { \
    if (prmIn.member != prmOut.member) { \
        ret = true;\
        PrintMes((prmIn.member == ignoreIfInput) ? RGY_LOG_DEBUG : RGY_LOG_WARN, _T("%s value changed %s -> %s by driver\n"), _T(#member), printMethod(prmIn.member), printMethod(prmOut.member)); \
    }}
#define COMPARE_LST(member, ignoreIfInput, list) { \
    if (prmIn.member != prmOut.member) { \
        ret = true;\
        PrintMes((prmIn.member == ignoreIfInput) ? RGY_LOG_DEBUG : RGY_LOG_WARN, _T("%s value changed %s -> %s by driver\n"), _T(#member), get_chr_from_value(list, prmIn.member), get_chr_from_value(list, prmOut.member)); \
    }}
    COMPARE_INT(vidprm.AsyncDepth,             0);
    COMPARE_HEX(vidprm.IOPattern,              0);
    COMPARE_INT(vidprm.mfx.NumThread,          0);
    COMPARE_INT(vidprm.mfx.BRCParamMultiplier, 0);
    COMPARE_INT(vidprm.mfx.LowPower,           0);
    COMPARE_STR(vidprm.mfx.CodecId,            0, CodecIdToStr);
    COMPARE_LST(vidprm.mfx.CodecProfile,       0, get_profile_list(prmIn.vidprm.mfx.CodecId));
    COMPARE_LST(vidprm.mfx.CodecLevel,         0, get_level_list(prmIn.vidprm.mfx.CodecId));
    COMPARE_INT(vidprm.mfx.NumThread,          0);
    COMPARE_INT(vidprm.mfx.TargetUsage,       -1);
    COMPARE_INT(vidprm.mfx.GopPicSize,         0);
    COMPARE_INT(vidprm.mfx.GopRefDist,         0);
    COMPARE_INT(vidprm.mfx.GopOptFlag,         0);
    COMPARE_INT(vidprm.mfx.IdrInterval,        0);
    COMPARE_STR(vidprm.mfx.RateControlMethod,  0, EncmodeToStr);
    if (prmIn.vidprm.mfx.RateControlMethod == MFX_RATECONTROL_CQP) {
        COMPARE_INT(vidprm.mfx.QPI, -1);
        COMPARE_INT(vidprm.mfx.QPP, -1);
        COMPARE_INT(vidprm.mfx.QPB, -1);
    } else if (rc_is_type_lookahead(m_mfxEncParams.mfx.RateControlMethod)) {
        COMPARE_INT(cop2.LookAheadDepth, -1);
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_8)) {
            COMPARE_LST(cop2.LookAheadDS, 0, list_lookahead_ds);
        }
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_11)) {
            COMPARE_INT(cop3.WinBRCSize,       0);
            COMPARE_INT(cop3.WinBRCMaxAvgKbps, 0);
        }
        if (MFX_RATECONTROL_LA_ICQ == m_mfxEncParams.mfx.RateControlMethod) {
            COMPARE_INT(vidprm.mfx.ICQQuality, -1);
        }
    } else if (MFX_RATECONTROL_ICQ == m_mfxEncParams.mfx.RateControlMethod) {
        COMPARE_INT(vidprm.mfx.ICQQuality, -1);
    } else {
        COMPARE_INT(vidprm.mfx.TargetKbps, 0);
        if (m_mfxEncParams.mfx.RateControlMethod == MFX_RATECONTROL_AVBR) {
            COMPARE_INT(vidprm.mfx.TargetKbps, 0);
        } else {
            COMPARE_INT(vidprm.mfx.MaxKbps, 0);
            if (m_mfxEncParams.mfx.RateControlMethod == MFX_RATECONTROL_QVBR) {
                COMPARE_INT(cop3.QVBRQuality, -1);
            }
        }
    }
    COMPARE_INT(vidprm.mfx.NumSlice,             0);
    COMPARE_INT(vidprm.mfx.NumRefFrame,          0);
    COMPARE_INT(vidprm.mfx.EncodedOrder,         0);
    COMPARE_INT(vidprm.mfx.ExtendedPicStruct,    0);
    COMPARE_INT(vidprm.mfx.TimeStampCalc,        0);
    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_6)) {
        COMPARE_INT(vidprm.mfx.SliceGroupsPresent, 0);
    }
    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_15)) {
        COMPARE_TRI(vidprm.mfx.LowPower, 0);
    }
    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_16)) {
        COMPARE_INT(vidprm.mfx.MaxDecFrameBuffering, 0);
    }

    COMPARE_TRI(cop.RateDistortionOpt,    0);
    COMPARE_INT(cop.MECostType,           0);
    COMPARE_INT(cop.MESearchType,         0);
    COMPARE_TRI(cop.EndOfSequence,        0);
    COMPARE_TRI(cop.FramePicture,         0);
    COMPARE_TRI(cop.CAVLC,                0);
    COMPARE_TRI(cop.ViewOutput,           0);
    COMPARE_TRI(cop.VuiVclHrdParameters,  0);
    COMPARE_TRI(cop.RefPicListReordering, 0);
    COMPARE_TRI(cop.ResetRefList,         0);
    COMPARE_INT(cop.MaxDecFrameBuffering, 0);
    COMPARE_TRI(cop.AUDelimiter,          0);
    COMPARE_TRI(cop.EndOfStream,          0);
    COMPARE_TRI(cop.PicTimingSEI,         0);
    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_3)) {
        COMPARE_TRI(cop.RefPicMarkRep,       0);
        COMPARE_TRI(cop.FieldOutput,         0);
        COMPARE_TRI(cop.NalHrdConformance,   0);
        COMPARE_TRI(cop.SingleSeiNalUnit,    0);
        COMPARE_TRI(cop.VuiNalHrdParameters, 0);
    }
    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_6)) {
        COMPARE_TRI(cop.RecoveryPointSEI, 0);

        COMPARE_INT(cop2.MaxFrameSize,    0);
        COMPARE_INT(cop2.MaxSliceSize,    0);
        COMPARE_TRI(cop2.BitrateLimit,    0);
        COMPARE_TRI(cop2.MBBRC,           0);
        COMPARE_TRI(cop2.ExtBRC,          0);
    }

    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_8)) {
        COMPARE_TRI(cop2.RepeatPPS,           0);
        COMPARE_INT(cop2.BRefType,            0);
        COMPARE_TRI(cop2.AdaptiveI,           0);
        COMPARE_TRI(cop2.AdaptiveB,           0);
        COMPARE_INT(cop2.NumMbPerSlice,       0);
    }
    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_9)) {
        COMPARE_INT(cop2.MaxSliceSize,        0);
        COMPARE_INT(cop2.SkipFrame,           0);
        COMPARE_INT(cop2.MinQPI,              0);
        COMPARE_INT(cop2.MaxQPI,              0);
        COMPARE_INT(cop2.MinQPP,              0);
        COMPARE_INT(cop2.MaxQPP,              0);
        COMPARE_INT(cop2.MinQPB,              0);
        COMPARE_INT(cop2.MaxQPB,              0);
        COMPARE_INT(cop2.FixedFrameRate,      0);
        COMPARE_INT(cop2.DisableDeblockingIdc,0);
    }
    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_10)) {
        COMPARE_INT(cop2.DisableVUI,         0);
        COMPARE_INT(cop2.BufferingPeriodSEI, 0);
    }
    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_11)) {
        COMPARE_TRI(cop2.EnableMAD, 0);
    }
    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_13)) {
        COMPARE_TRI(cop2.UseRawRef, 0);
    }

    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_11)) {
        COMPARE_INT(cop3.NumSliceI,                  0);
        COMPARE_INT(cop3.NumSliceP,                  0);
        COMPARE_INT(cop3.NumSliceB,                  0);
        if (rc_is_type_lookahead(m_mfxEncParams.mfx.RateControlMethod)) {
            COMPARE_INT(cop3.WinBRCMaxAvgKbps,       0);
            COMPARE_INT(cop3.WinBRCSize,             0);
        }
    }
    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_13)) {
        COMPARE_TRI(cop3.EnableMBQP,                 0);
        COMPARE_TRI(cop3.DirectBiasAdjustment,       0);
        COMPARE_TRI(cop3.GlobalMotionBiasAdjustment, 0);
        COMPARE_INT(cop3.MVCostScalingFactor,        0);
    }
    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_16)) {
        COMPARE_INT(cop3.IntRefCycleDist,            0);
        COMPARE_TRI(cop3.MBDisableSkipMap,           0);
        COMPARE_INT(cop3.WeightedPred,               0);
        COMPARE_INT(cop3.WeightedBiPred,             0);
        COMPARE_TRI(cop3.AspectRatioInfoPresent,     0);
        COMPARE_TRI(cop3.OverscanInfoPresent,        0);
        COMPARE_TRI(cop3.OverscanAppropriate,        0);
        COMPARE_TRI(cop3.TimingInfoPresent,          0);
        COMPARE_TRI(cop3.BitstreamRestriction,       0);
        COMPARE_INT(cop3.PRefType,                   0);
    }
    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_17)) {
        COMPARE_TRI(cop3.FadeDetection,              0);
    }
    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_19)) {
        COMPARE_TRI(cop3.LowDelayHrd,                0);
        COMPARE_TRI(cop3.MotionVectorsOverPicBoundaries, 0);
        COMPARE_TRI(cop3.MaxFrameSizeI,      0);
        COMPARE_TRI(cop3.MaxFrameSizeP,      0);
        COMPARE_TRI(cop3.EnableQPOffset,     0);
        COMPARE_TRI(cop3.TransformSkip,      0);
        COMPARE_INT(cop3.QPOffset[0],        0);
        COMPARE_INT(cop3.QPOffset[1],        0);
        COMPARE_INT(cop3.QPOffset[2],        0);
        COMPARE_INT(cop3.NumRefActiveP[0],   0);
        COMPARE_INT(cop3.NumRefActiveP[1],   0);
        COMPARE_INT(cop3.NumRefActiveP[2],   0);
        COMPARE_INT(cop3.NumRefActiveBL0[0], 0);
        COMPARE_INT(cop3.NumRefActiveBL0[1], 0);
        COMPARE_INT(cop3.NumRefActiveBL0[2], 0);
        COMPARE_INT(cop3.NumRefActiveBL1[0], 0);
        COMPARE_INT(cop3.NumRefActiveBL1[1], 0);
        COMPARE_INT(cop3.NumRefActiveBL1[2], 0);
    }
    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_26)) {
        COMPARE_TRI(hevc.SampleAdaptiveOffset,  MFX_SAO_UNKNOWN);
        COMPARE_TRI(hevc.LCUSize, 0);
    }
    return ret;
}

//範囲チェック
RGY_ERR CQSVPipeline::CheckParamList(int value, const CX_DESC *list, const char *param_name) {
    for (int i = 0; list[i].desc; i++)
        if (list[i].value == value)
            return RGY_ERR_NONE;
    PrintMes(RGY_LOG_ERROR, _T("%s=%d, is not valid param.\n"), param_name, value);
    return RGY_ERR_INVALID_VIDEO_PARAM;
};

RGY_ERR CQSVPipeline::InitMfxDecParams(sInputParams *pInParams) {
#if ENABLE_AVSW_READER
    RGY_ERR sts = RGY_ERR_NONE;
    if (m_pFileReader->getInputCodec()) {
        m_DecInputBitstream.init(AVCODEC_READER_INPUT_BUF_SIZE);
        //TimeStampはQSVに自動的に計算させる
        m_DecInputBitstream.setPts(MFX_TIMESTAMP_UNKNOWN);

        sts = m_pFileReader->GetHeader(&m_DecInputBitstream);
        RGY_ERR(sts, _T("InitMfxDecParams: Failed to get stream header from reader."));

        const bool bGotHeader = m_DecInputBitstream.size() > 0;
        if (!bGotHeader) {
            //最初のフレームそのものをヘッダーとして使用する。
            //ここで読み込みんだ第1フレームのデータを読み込み側から消してしまうと、
            //メインループでは第2フレームのデータがmfxBitstreamに追加されてしまい、
            //第1・第2フレームの両方のデータが存在することになってしまう。
            //VP8/VP9のデコードでは、mfxBitstreamに複数のフレームのデータがあるとうまく動作しないことがあるためこれを回避する。
            //ここで読み込んだ第1フレームは読み込み側から消さないようにすることで、
            //メインループで再び第1フレームのデータとして読み込むことができる。
            m_pFileReader->GetNextBitstreamNoDelete(&m_DecInputBitstream);
        }

        //デコーダの作成
        mfxIMPL impl;
        m_mfxSession.QueryIMPL(&impl);
        m_mfxDEC = std::make_unique<QSVMfxDec>(m_hwdev.get(), m_pMFXAllocator.get(), m_mfxVer, impl, m_memType, m_pQSVLog);

        sts = m_mfxDEC->InitSession();
        RGY_ERR(sts, _T("InitMfxDecParams: Failed init session for hw decoder."));

        sts = m_mfxDEC->SetParam(m_pFileReader->getInputCodec(), m_DecInputBitstream, m_pFileReader->GetInputFrameInfo());
        RGY_ERR(sts, _T("InitMfxDecParams: Failed set param for hw decoder."));

        if (!bGotHeader) {
            //最初のフレームそのものをヘッダーとして使用している場合、一度データをクリアする
            //メインループに入った際に再度第1フレームを読み込むようにする。
            m_DecInputBitstream.clear();
        }
    }
#endif
    return RGY_ERR_NONE;
}

RGY_ERR CQSVPipeline::InitMfxEncodeParams(sInputParams *pInParams) {
    if (pInParams->CodecId == MFX_CODEC_RAW) {
        PrintMes(RGY_LOG_DEBUG, _T("Raw codec is selected, disable encode.\n"));
        return RGY_ERR_NONE;
    }
    const mfxU32 blocksz = (pInParams->CodecId == MFX_CODEC_HEVC) ? 32 : 16;
    auto print_feature_warnings = [this](int log_level, const TCHAR *feature_name) {
        PrintMes(log_level, _T("%s is not supported on current platform, disabled.\n"), feature_name);
    };

    auto sts = err_to_rgy(m_SessionPlugins->LoadPlugin(MFXComponentType::ENCODE, pInParams->CodecId, false));
    if (sts != RGY_ERR_NONE) {
        PrintMes(RGY_LOG_ERROR, _T("Failed to load hw %s encoder.\n"), CodecIdToStr(pInParams->CodecId));
        PrintMes(RGY_LOG_ERROR, _T("%s encoding is not supported on current platform.\n"), CodecIdToStr(pInParams->CodecId));
        return RGY_ERR_UNSUPPORTED;
    }
    const int encodeBitDepth = getEncoderBitdepth(pInParams);
    if (encodeBitDepth <= 0) {
        PrintMes(RGY_LOG_ERROR, _T("Unknown codec.\n"));
        return RGY_ERR_UNSUPPORTED;
    }
    const int codecMaxQP = 51 + (encodeBitDepth - 8) * 6;
    PrintMes(RGY_LOG_DEBUG, _T("encodeBitDepth: %d, codecMaxQP: %d.\n"), encodeBitDepth, codecMaxQP);

    //エンコードモードのチェック
    auto availableFeaures = CheckEncodeFeature(m_mfxSession, m_mfxVer, pInParams->nEncMode, pInParams->CodecId);
    PrintMes(RGY_LOG_DEBUG, _T("Detected avaliable features for hw API v%d.%d, %s, %s\n%s\n"),
        m_mfxVer.Major, m_mfxVer.Minor,
        CodecIdToStr(pInParams->CodecId), EncmodeToStr(pInParams->nEncMode), MakeFeatureListStr(availableFeaures).c_str());
    if (!(availableFeaures & ENC_FEATURE_CURRENT_RC)) {
        //このコーデックがサポートされているかどうか確認する
        if (   pInParams->nEncMode == MFX_RATECONTROL_CQP
            || pInParams->nEncMode == MFX_RATECONTROL_VBR
            || pInParams->nEncMode == MFX_RATECONTROL_CBR
            || !(CheckEncodeFeature(m_mfxSession, m_mfxVer, MFX_RATECONTROL_CQP, pInParams->CodecId) & ENC_FEATURE_CURRENT_RC)) {
            PrintMes(RGY_LOG_ERROR, _T("%s encoding is not supported on current platform.\n"), CodecIdToStr(pInParams->CodecId));
            return RGY_ERR_INVALID_VIDEO_PARAM;
        }
        const int rc_error_log_level = (pInParams->nFallback) ? RGY_LOG_WARN : RGY_LOG_ERROR;
        PrintMes(rc_error_log_level, _T("%s mode is not supported on current platform.\n"), EncmodeToStr(pInParams->nEncMode));
        if (MFX_RATECONTROL_LA == pInParams->nEncMode) {
            if (!check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_7)) {
                PrintMes(rc_error_log_level, _T("Lookahead mode is only supported by API v1.7 or later.\n"));
            }
        }
        if (   MFX_RATECONTROL_ICQ    == pInParams->nEncMode
            || MFX_RATECONTROL_LA_ICQ == pInParams->nEncMode
            || MFX_RATECONTROL_VCM    == pInParams->nEncMode) {
            if (!check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_8)) {
                PrintMes(rc_error_log_level, _T("%s mode is only supported by API v1.8 or later.\n"), EncmodeToStr(pInParams->nEncMode));
            }
        }
        if (   MFX_RATECONTROL_LA_EXT == pInParams->nEncMode
            || MFX_RATECONTROL_LA_HRD == pInParams->nEncMode
            || MFX_RATECONTROL_QVBR   == pInParams->nEncMode) {
            if (!check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_11)) {
                PrintMes(rc_error_log_level, _T("%s mode is only supported by API v1.11 or later.\n"), EncmodeToStr(pInParams->nEncMode));
            }
        }
        if (!pInParams->nFallback) {
            return RGY_ERR_INVALID_VIDEO_PARAM;
        }
        //fallback
        //fallbackの候補リスト、優先度の高い順にセットする
        vector<int> check_rc_list;
        //現在のレート制御モードは使用できないので、それ以外を確認する
        auto check_rc_add = [pInParams, &check_rc_list](int rc_mode) {
            if (pInParams->nEncMode != rc_mode) {
                check_rc_list.push_back(rc_mode);
            }
        };

        //品質指定系の場合、若干補正をかけた値を設定する
        int nAdjustedQP[3] = { QSV_DEFAULT_QPI, QSV_DEFAULT_QPP, QSV_DEFAULT_QPB };
        if (isRCBitrateMode(pInParams->nEncMode)) {
            //ビットレートモードなら、QVBR->VBRをチェックする
            check_rc_add(MFX_RATECONTROL_QVBR);
            check_rc_add(MFX_RATECONTROL_VBR);
        } else {
            //固定品質モードなら、ICQ->CQPをチェックする
            check_rc_add(MFX_RATECONTROL_ICQ);
            check_rc_add(MFX_RATECONTROL_CQP);
            //品質指定系の場合、若干補正をかけた値を設定する
            if (pInParams->nEncMode == MFX_RATECONTROL_LA_ICQ) {
                nAdjustedQP[0] = pInParams->nICQQuality - 8;
                nAdjustedQP[1] = pInParams->nICQQuality - 6;
                nAdjustedQP[2] = pInParams->nICQQuality - 3;
            } else if (pInParams->nEncMode == MFX_RATECONTROL_ICQ) {
                nAdjustedQP[0] = pInParams->nICQQuality - 1;
                nAdjustedQP[1] = pInParams->nICQQuality + 1;
                nAdjustedQP[2] = pInParams->nICQQuality + 4;
            } else if (pInParams->nEncMode == MFX_RATECONTROL_CQP) {
                nAdjustedQP[0] = pInParams->nQPI;
                nAdjustedQP[1] = pInParams->nQPP;
                nAdjustedQP[2] = pInParams->nQPB;
            }
        }
        //check_rc_listに設定したfallbackの候補リストをチェックする
        bool bFallbackSuccess = false;
        for (uint32_t i = 0; i < (uint32_t)check_rc_list.size(); i++) {
            auto availRCFeatures = CheckEncodeFeature(m_mfxSession, m_mfxVer, (uint16_t)check_rc_list[i], pInParams->CodecId);
            if (availRCFeatures & ENC_FEATURE_CURRENT_RC) {
                pInParams->nEncMode = (uint16_t)check_rc_list[i];
                if (pInParams->nEncMode == MFX_RATECONTROL_LA_ICQ) {
                    pInParams->nICQQuality = (uint16_t)clamp(nAdjustedQP[1] + 6, 1, codecMaxQP);
                } else if (pInParams->nEncMode == MFX_RATECONTROL_LA_ICQ) {
                    pInParams->nICQQuality = (uint16_t)clamp(nAdjustedQP[1], 1, codecMaxQP);
                } else if (pInParams->nEncMode == MFX_RATECONTROL_CQP) {
                    pInParams->nQPI = (uint16_t)clamp(nAdjustedQP[0], 0, codecMaxQP);
                    pInParams->nQPP = (uint16_t)clamp(nAdjustedQP[1], 0, codecMaxQP);
                    pInParams->nQPB = (uint16_t)clamp(nAdjustedQP[2], 0, codecMaxQP);
                }
                bFallbackSuccess = true;
                availableFeaures = availRCFeatures;
                PrintMes(rc_error_log_level, _T("Falling back to %s mode.\n"), EncmodeToStr(pInParams->nEncMode));
                break;
            }
        }
        //なんらかの理由でフォールバックできなかったらエラー終了
        if (!bFallbackSuccess) {
            return RGY_ERR_INVALID_VIDEO_PARAM;
        }
    }
    if (pInParams->nBframes == QSV_BFRAMES_AUTO) {
        pInParams->nBframes = (pInParams->CodecId == MFX_CODEC_HEVC) ? QSV_DEFAULT_HEVC_BFRAMES : QSV_DEFAULT_H264_BFRAMES;
    }
    //その他機能のチェック
    if (pInParams->bAdaptiveI && !(availableFeaures & ENC_FEATURE_ADAPTIVE_I)) {
        PrintMes(RGY_LOG_WARN, _T("Adaptve I-frame insert is not supported on current platform, disabled.\n"));
        pInParams->bAdaptiveI = false;
    }
    if (pInParams->bAdaptiveB && !(availableFeaures & ENC_FEATURE_ADAPTIVE_B)) {
        PrintMes(RGY_LOG_WARN, _T("Adaptve B-frame insert is not supported on current platform, disabled.\n"));
        pInParams->bAdaptiveB = false;
    }
    if (pInParams->bBPyramid && !(availableFeaures & ENC_FEATURE_B_PYRAMID)) {
        print_feature_warnings(RGY_LOG_WARN, _T("B pyramid"));
        pInParams->bBPyramid = false;
    }
    if (pInParams->bCAVLC && !(availableFeaures & ENC_FEATURE_CAVLC)) {
        print_feature_warnings(RGY_LOG_WARN, _T("CAVLC"));
        pInParams->bCAVLC = false;
    }
    if (pInParams->extBRC && !(availableFeaures & ENC_FEATURE_EXT_BRC)) {
        print_feature_warnings(RGY_LOG_WARN, _T("ExtBRC"));
        pInParams->extBRC = false;
    }
    if (pInParams->extBrcAdaptiveLTR && !(availableFeaures & ENC_FEATURE_EXT_BRC_ADAPTIVE_LTR)) {
        print_feature_warnings(RGY_LOG_WARN, _T("AdaptiveLTR"));
        pInParams->extBrcAdaptiveLTR = false;
    }
    if (pInParams->bMBBRC && !(availableFeaures & ENC_FEATURE_MBBRC)) {
        print_feature_warnings(RGY_LOG_WARN, _T("MBBRC"));
        pInParams->bMBBRC = false;
    }
    if (   (MFX_RATECONTROL_LA     == pInParams->nEncMode
         || MFX_RATECONTROL_LA_ICQ == pInParams->nEncMode)
        && pInParams->nLookaheadDS != MFX_LOOKAHEAD_DS_UNKNOWN
        && !(availableFeaures & ENC_FEATURE_LA_DS)) {
        print_feature_warnings(RGY_LOG_WARN, _T("Lookahead qaulity setting"));
        pInParams->nLookaheadDS = MFX_LOOKAHEAD_DS_UNKNOWN;
    }
    if (pInParams->nTrellis != MFX_TRELLIS_UNKNOWN && !(availableFeaures & ENC_FEATURE_TRELLIS)) {
        print_feature_warnings(RGY_LOG_WARN, _T("trellis"));
        pInParams->nTrellis = MFX_TRELLIS_UNKNOWN;
    }
    if (pInParams->bRDO && !(availableFeaures & ENC_FEATURE_RDO)) {
        print_feature_warnings(RGY_LOG_WARN, _T("RDO"));
        pInParams->bRDO = false;
    }
    if (((m_encPicstruct & RGY_PICSTRUCT_INTERLACED) != 0)
        && !(availableFeaures & ENC_FEATURE_INTERLACE)) {
        PrintMes(RGY_LOG_ERROR, _T("Interlaced encoding is not supported on current rate control mode.\n"));
        return RGY_ERR_INVALID_VIDEO_PARAM;
    }
    if (pInParams->CodecId == MFX_CODEC_AVC
        && ((m_encPicstruct & RGY_PICSTRUCT_INTERLACED) != 0)
        && pInParams->nBframes > 0
        && getCPUGen(&m_mfxSession) == CPU_GEN_HASWELL
        && m_memType == D3D11_MEMORY) {
        PrintMes(RGY_LOG_WARN, _T("H.264 interlaced encoding with B frames on d3d11 mode results fuzzy outputs on Haswell CPUs.\n"));
        PrintMes(RGY_LOG_WARN, _T("B frames will be disabled.\n"));
        pInParams->nBframes = 0;
    }
    //最近のドライバでは問題ない模様
    //if (pInParams->nBframes > 2 && pInParams->CodecId == MFX_CODEC_HEVC) {
    //    PrintMes(RGY_LOG_WARN, _T("HEVC encoding + B-frames > 2 might cause artifacts, please check the output.\n"));
    //}
    if (pInParams->bBPyramid && pInParams->nBframes >= 10 && !(availableFeaures & ENC_FEATURE_B_PYRAMID_MANY_BFRAMES)) {
        PrintMes(RGY_LOG_WARN, _T("B pyramid with too many bframes is not supported on current platform, B pyramid disabled.\n"));
        pInParams->bBPyramid = false;
    }
    if (pInParams->bBPyramid && getCPUGen(&m_mfxSession) < CPU_GEN_HASWELL) {
        PrintMes(RGY_LOG_WARN, _T("B pyramid on IvyBridge generation might cause artifacts, please check your encoded video.\n"));
    }
    if (pInParams->bNoDeblock && !(availableFeaures & ENC_FEATURE_NO_DEBLOCK)) {
        print_feature_warnings(RGY_LOG_WARN, _T("No deblock"));
        pInParams->bNoDeblock = false;
    }
    if (pInParams->bIntraRefresh && !(availableFeaures & ENC_FEATURE_INTRA_REFRESH)) {
        print_feature_warnings(RGY_LOG_WARN, _T("Intra Refresh"));
        pInParams->bIntraRefresh = false;
    }
    if (0 != (pInParams->nQPMin[0] | pInParams->nQPMin[1] | pInParams->nQPMin[2]
            | pInParams->nQPMax[0] | pInParams->nQPMax[1] | pInParams->nQPMax[2]) && !(availableFeaures & ENC_FEATURE_QP_MINMAX)) {
        print_feature_warnings(RGY_LOG_WARN, _T("Min/Max QP"));
        memset(pInParams->nQPMin, 0, sizeof(pInParams->nQPMin));
        memset(pInParams->nQPMax, 0, sizeof(pInParams->nQPMax));
    }
    if (0 != pInParams->nWinBRCSize) {
        if (!(availableFeaures & ENC_FEATURE_WINBRC)) {
            print_feature_warnings(RGY_LOG_WARN, _T("WinBRC"));
            pInParams->nWinBRCSize = 0;
        } else if (0 == pInParams->nMaxBitrate) {
            print_feature_warnings(RGY_LOG_WARN, _T("Min/Max QP"));
            PrintMes(RGY_LOG_WARN, _T("WinBRC requires Max bitrate to be set, disabled.\n"));
            pInParams->nWinBRCSize = 0;
        }
    }
    if (pInParams->bDirectBiasAdjust && !(availableFeaures & ENC_FEATURE_DIRECT_BIAS_ADJUST)) {
        print_feature_warnings(RGY_LOG_WARN, _T("Direct Bias Adjust"));
        pInParams->bDirectBiasAdjust = 0;
    }
    if (pInParams->bGlobalMotionAdjust && !(availableFeaures & ENC_FEATURE_GLOBAL_MOTION_ADJUST)) {
        print_feature_warnings(RGY_LOG_WARN, _T("MV Cost Scaling"));
        pInParams->bGlobalMotionAdjust = 0;
        pInParams->nMVCostScaling = 0;
    }
    if (pInParams->bUseFixedFunc && !(availableFeaures & ENC_FEATURE_FIXED_FUNC)) {
        print_feature_warnings(RGY_LOG_WARN, _T("Fixed Func"));
        pInParams->bUseFixedFunc = 0;
    }
    if (pInParams->nWeightP && !(availableFeaures & ENC_FEATURE_WEIGHT_P)) {
        if (pInParams->nWeightP == MFX_CODINGOPTION_ON) {
            print_feature_warnings(RGY_LOG_WARN, _T("WeightP"));
        }
        pInParams->nWeightP = 0;
    }
    if (pInParams->nWeightB && !(availableFeaures & ENC_FEATURE_WEIGHT_B)) {
        if (pInParams->nWeightB == MFX_CODINGOPTION_ON) {
            print_feature_warnings(RGY_LOG_WARN, _T("WeightB"));
        }
        pInParams->nWeightB = 0;
    }
#if !ENABLE_FADE_DETECT
    if (pInParams->nFadeDetect == MFX_CODINGOPTION_ON) {
        PrintMes(RGY_LOG_WARN, _T("fade-detect will be disabled due to instability.\n"));
        pInParams->nFadeDetect = MFX_CODINGOPTION_UNKNOWN;
    }
#endif
    if (pInParams->nFadeDetect != MFX_CODINGOPTION_UNKNOWN && !(availableFeaures & ENC_FEATURE_FADE_DETECT)) {
        if (pInParams->nFadeDetect == MFX_CODINGOPTION_ON) {
            print_feature_warnings(RGY_LOG_WARN, _T("FadeDetect"));
        }
        pInParams->nFadeDetect = MFX_CODINGOPTION_UNKNOWN;
    }
    if (pInParams->CodecId == MFX_CODEC_HEVC) {
        if (pInParams->hevc_ctu > 0 && !(availableFeaures & ENC_FEATURE_HEVC_CTU)) {
            print_feature_warnings(RGY_LOG_WARN, _T("HEVC CTU"));
            pInParams->hevc_ctu = 0;
        }
        if (pInParams->hevc_sao != MFX_SAO_UNKNOWN && !(availableFeaures & ENC_FEATURE_HEVC_SAO)) {
            print_feature_warnings(RGY_LOG_WARN, _T("HEVC SAO"));
            pInParams->hevc_sao = MFX_SAO_UNKNOWN;
        }
        if (pInParams->hevc_tskip != MFX_CODINGOPTION_UNKNOWN && !(availableFeaures & ENC_FEATURE_HEVC_TSKIP)) {
            print_feature_warnings(RGY_LOG_WARN, _T("HEVC tskip"));
            pInParams->hevc_tskip = MFX_CODINGOPTION_UNKNOWN;
        }
    }
    bool bQPOffsetUsed = false;
    std::for_each(pInParams->pQPOffset, pInParams->pQPOffset + _countof(pInParams->pQPOffset), [&bQPOffsetUsed](decltype(pInParams->pQPOffset[0]) v){ bQPOffsetUsed |= (v != 0); });
    if (bQPOffsetUsed && !(availableFeaures & ENC_FEATURE_PYRAMID_QP_OFFSET)) {
        print_feature_warnings(RGY_LOG_WARN, _T("QPOffset"));
        memset(pInParams->pQPOffset, 0, sizeof(pInParams->pQPOffset));
        bQPOffsetUsed = false;
    }

    if (!(availableFeaures & ENC_FEATURE_VUI_INFO)) {
        if (m_encVUI.colorrange == RGY_COLORRANGE_FULL) {
            print_feature_warnings(RGY_LOG_WARN, _T("fullrange"));
            m_encVUI.colorrange = RGY_COLORRANGE_UNSPECIFIED;
        }
        if (m_encVUI.transfer != get_cx_value(list_transfer, _T("undef"))) {
            print_feature_warnings(RGY_LOG_WARN, _T("transfer"));
            m_encVUI.transfer = (CspTransfer)get_cx_value(list_transfer, _T("undef"));
        }
        if (m_encVUI.format != get_cx_value(list_videoformat, _T("undef"))) {
            print_feature_warnings(RGY_LOG_WARN, _T("videoformat"));
            m_encVUI.format = get_cx_value(list_videoformat, _T("undef"));
        }
        if (m_encVUI.matrix != get_cx_value(list_colormatrix, _T("undef"))) {
            print_feature_warnings(RGY_LOG_WARN, _T("colormatrix"));
            m_encVUI.matrix = (CspMatrix)get_cx_value(list_colormatrix, _T("undef"));
        }
        if (m_encVUI.colorprim != get_cx_value(list_colorprim, _T("undef"))) {
            print_feature_warnings(RGY_LOG_WARN, _T("colorprim"));
            m_encVUI.colorprim = (CspColorprim)get_cx_value(list_colorprim, _T("undef"));
        }
    }
    m_encVUI.descriptpresent =
           (int)m_encVUI.matrix != get_cx_value(list_colormatrix, _T("undef"))
        || (int)m_encVUI.colorprim != get_cx_value(list_colorprim, _T("undef"))
        || (int)m_encVUI.transfer != get_cx_value(list_transfer, _T("undef"));

    //Intra Refereshが指定された場合は、GOP関連の設定を自動的に上書き
    if (pInParams->bIntraRefresh) {
        pInParams->bforceGOPSettings = true;
    }
    //profileを守るための調整
    if (pInParams->CodecProfile == MFX_PROFILE_AVC_BASELINE) {
        pInParams->nBframes = 0;
        pInParams->bCAVLC = true;
    }
    if (pInParams->bCAVLC) {
        pInParams->bRDO = false;
    }

    CHECK_RANGE_LIST(pInParams->CodecId,      list_codec,   "codec");
    CHECK_RANGE_LIST(pInParams->CodecLevel,   get_level_list(pInParams->CodecId),   "level");
    CHECK_RANGE_LIST(pInParams->CodecProfile, get_profile_list(pInParams->CodecId), "profile");
    CHECK_RANGE_LIST(pInParams->nEncMode,     list_rc_mode, "rc mode");

    //設定開始
    m_mfxEncParams.mfx.CodecId                 = pInParams->CodecId;
    m_mfxEncParams.mfx.RateControlMethod       = (mfxU16)pInParams->nEncMode;
    if (MFX_RATECONTROL_CQP == m_mfxEncParams.mfx.RateControlMethod) {
        //CQP
        m_mfxEncParams.mfx.QPI             = (mfxU16)clamp_param_int(pInParams->nQPI, 0, codecMaxQP, _T("qp-i"));
        m_mfxEncParams.mfx.QPP             = (mfxU16)clamp_param_int(pInParams->nQPP, 0, codecMaxQP, _T("qp-p"));
        m_mfxEncParams.mfx.QPB             = (mfxU16)clamp_param_int(pInParams->nQPB, 0, codecMaxQP, _T("qp-b"));
    } else if (MFX_RATECONTROL_ICQ    == m_mfxEncParams.mfx.RateControlMethod
            || MFX_RATECONTROL_LA_ICQ == m_mfxEncParams.mfx.RateControlMethod) {
        m_mfxEncParams.mfx.ICQQuality      = (mfxU16)clamp_param_int(pInParams->nICQQuality, 1, codecMaxQP, _T("icq"));
        m_mfxEncParams.mfx.MaxKbps         = 0;
    } else {
        auto maxBitrate = (std::max)((std::max)(pInParams->nBitRate, pInParams->nMaxBitrate),
            pInParams->VBVBufsize / 8 /*これはbyte単位の指定*/);
        if (maxBitrate > USHRT_MAX) {
            m_mfxEncParams.mfx.BRCParamMultiplier = (mfxU16)(maxBitrate / USHRT_MAX) + 1;
            pInParams->nBitRate    /= m_mfxEncParams.mfx.BRCParamMultiplier;
            pInParams->nMaxBitrate /= m_mfxEncParams.mfx.BRCParamMultiplier;
            pInParams->VBVBufsize  /= m_mfxEncParams.mfx.BRCParamMultiplier;
        }
        m_mfxEncParams.mfx.TargetKbps      = (mfxU16)pInParams->nBitRate; // in kbps
        if (m_mfxEncParams.mfx.RateControlMethod == MFX_RATECONTROL_AVBR) {
            //AVBR
            //m_mfxEncParams.mfx.Accuracy        = pInParams->nAVBRAccuarcy;
            m_mfxEncParams.mfx.Accuracy        = 500;
            m_mfxEncParams.mfx.Convergence     = (mfxU16)pInParams->nAVBRConvergence;
        } else {
            //CBR, VBR
            m_mfxEncParams.mfx.MaxKbps         = (mfxU16)pInParams->nMaxBitrate;
            m_mfxEncParams.mfx.BufferSizeInKB  = (mfxU16)pInParams->VBVBufsize / 8; //これはbyte単位の指定
            m_mfxEncParams.mfx.InitialDelayInKB = m_mfxEncParams.mfx.BufferSizeInKB / 2;
        }
    }
    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_15)) {
        m_mfxEncParams.mfx.LowPower = (mfxU16)((pInParams->bUseFixedFunc) ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF);
    }
    m_mfxEncParams.mfx.TargetUsage             = (mfxU16)clamp_param_int(pInParams->nTargetUsage, MFX_TARGETUSAGE_BEST_QUALITY, MFX_TARGETUSAGE_BEST_SPEED, _T("quality")); // trade-off between quality and speed

    PrintMes(RGY_LOG_DEBUG, _T("InitMfxEncParams: Output FPS %d/%d\n"), m_encFps.n(), m_encFps.d());
    if (pInParams->nGOPLength == 0) {
        pInParams->nGOPLength = (mfxU16)((m_encFps.n() + m_encFps.d() - 1) / m_encFps.d()) * 10;
        PrintMes(RGY_LOG_DEBUG, _T("InitMfxEncParams: Auto GOP Length: %d\n"), pInParams->nGOPLength);
    }
    m_mfxEncParams.mfx.FrameInfo.FrameRateExtN = m_encFps.n();
    m_mfxEncParams.mfx.FrameInfo.FrameRateExtD = m_encFps.d();
    m_mfxEncParams.mfx.EncodedOrder            = 0;
    m_mfxEncParams.mfx.NumSlice                = (mfxU16)pInParams->nSlices;

    m_mfxEncParams.mfx.NumRefFrame             = (mfxU16)clamp_param_int(pInParams->nRef, 0, 16, _T("ref"));
    m_mfxEncParams.mfx.CodecLevel              = (mfxU16)pInParams->CodecLevel;
    m_mfxEncParams.mfx.CodecProfile            = (mfxU16)pInParams->CodecProfile;
    m_mfxEncParams.mfx.GopOptFlag              = 0;
    m_mfxEncParams.mfx.GopOptFlag             |= (!pInParams->bopenGOP) ? MFX_GOP_CLOSED : 0x00;

    /* For H.264, IdrInterval specifies IDR-frame interval in terms of I-frames; if IdrInterval = 0, then every I-frame is an IDR-frame. If IdrInterval = 1, then every other I-frame is an IDR-frame, etc.
     * For HEVC, if IdrInterval = 0, then only first I-frame is an IDR-frame. If IdrInterval = 1, then every I-frame is an IDR-frame. If IdrInterval = 2, then every other I-frame is an IDR-frame, etc.
     * For MPEG2, IdrInterval defines sequence header interval in terms of I-frames. If IdrInterval = N, SDK inserts the sequence header before every Nth I-frame. If IdrInterval = 0 (default), SDK inserts the sequence header once at the beginning of the stream.
     * If GopPicSize or GopRefDist is zero, IdrInterval is undefined. */
    if (pInParams->CodecId == MFX_CODEC_HEVC) {
        m_mfxEncParams.mfx.IdrInterval = (mfxU16)((!pInParams->bopenGOP) ? 1 : 1 + ((m_encFps.n() + m_encFps.d() - 1) / m_encFps.d()) * 20 / pInParams->nGOPLength);
    } else if (pInParams->CodecId == MFX_CODEC_AVC) {
        m_mfxEncParams.mfx.IdrInterval = (mfxU16)((!pInParams->bopenGOP) ? 0 : ((m_encFps.n() + m_encFps.d() - 1) / m_encFps.d()) * 20 / pInParams->nGOPLength);
    } else {
        m_mfxEncParams.mfx.IdrInterval = 0;
    }
    //MFX_GOP_STRICTにより、インタレ保持時にフレームが壊れる場合があるため、無効とする
    //m_mfxEncParams.mfx.GopOptFlag             |= (pInParams->bforceGOPSettings) ? MFX_GOP_STRICT : NULL;

    m_mfxEncParams.mfx.GopPicSize              = (pInParams->bIntraRefresh) ? 0 : (mfxU16)pInParams->nGOPLength;
    m_mfxEncParams.mfx.GopRefDist              = (mfxU16)(clamp_param_int(pInParams->nBframes, -1, 16, _T("bframes")) + 1);

    // specify memory type
    m_mfxEncParams.IOPattern = (mfxU16)((pInParams->memType != SYSTEM_MEMORY) ? MFX_IOPATTERN_IN_VIDEO_MEMORY : MFX_IOPATTERN_IN_SYSTEM_MEMORY);

    // frame info parameters
    m_mfxEncParams.mfx.FrameInfo.ChromaFormat = (mfxU16)chromafmt_rgy_to_enc(RGY_CSP_CHROMA_FORMAT[getEncoderCsp(pInParams)]);
    m_mfxEncParams.mfx.FrameInfo.PicStruct    = (mfxU16)picstruct_rgy_to_enc(m_encPicstruct);

    // set sar info
    auto par = std::make_pair(pInParams->nPAR[0], pInParams->nPAR[1]);
    if ((!pInParams->nPAR[0] || !pInParams->nPAR[1]) //SAR比の指定がない
        && pInParams->input.sar[0] && pInParams->input.sar[1] //入力側からSAR比を取得ずみ
        && (pInParams->input.dstWidth == pInParams->input.srcWidth && pInParams->input.dstHeight == pInParams->input.srcHeight)) {//リサイズは行われない
        par = std::make_pair(pInParams->input.sar[0], pInParams->input.sar[1]);
    }
    adjust_sar(&par.first, &par.second, pInParams->input.dstWidth, pInParams->input.dstHeight);
    m_mfxEncParams.mfx.FrameInfo.AspectRatioW = (mfxU16)par.first;
    m_mfxEncParams.mfx.FrameInfo.AspectRatioH = (mfxU16)par.second;

    RGY_MEMSET_ZERO(m_CodingOption);
    m_CodingOption.Header.BufferId = MFX_EXTBUFF_CODING_OPTION;
    m_CodingOption.Header.BufferSz = sizeof(mfxExtCodingOption);
    //if (!pInParams->bUseHWLib) {
    //    //swライブラリ使用時のみ
    //    m_CodingOption.InterPredBlockSize = pInParams->nInterPred;
    //    m_CodingOption.IntraPredBlockSize = pInParams->nIntraPred;
    //    m_CodingOption.MVSearchWindow     = pInParams->MVSearchWindow;
    //    m_CodingOption.MVPrecision        = pInParams->nMVPrecision;
    //}
    //if (!pInParams->bUseHWLib || pInParams->CodecProfile == MFX_PROFILE_AVC_BASELINE) {
    //    //swライブラリ使用時かbaselineを指定した時
    //    m_CodingOption.RateDistortionOpt  = (mfxU16)((pInParams->bRDO) ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_UNKNOWN);
    //    m_CodingOption.CAVLC              = (mfxU16)((pInParams->bCAVLC) ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_UNKNOWN);
    //}
    //m_CodingOption.FramePicture = MFX_CODINGOPTION_ON;
    //m_CodingOption.FieldOutput = MFX_CODINGOPTION_ON;
    //m_CodingOption.VuiVclHrdParameters = MFX_CODINGOPTION_ON;
    //m_CodingOption.VuiNalHrdParameters = MFX_CODINGOPTION_ON;
    m_CodingOption.AUDelimiter = (mfxU16)((pInParams->bOutputAud) ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF);
    m_CodingOption.PicTimingSEI = (mfxU16)((pInParams->bOutputPicStruct) ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF);
    //m_CodingOption.SingleSeiNalUnit = MFX_CODINGOPTION_OFF;

    //API v1.6の機能
    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_6)) {
        INIT_MFX_EXT_BUFFER(m_CodingOption2, MFX_EXTBUFF_CODING_OPTION2);
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_8)) {
            m_CodingOption2.AdaptiveI   = (mfxU16)((pInParams->bAdaptiveI) ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_UNKNOWN);
            m_CodingOption2.AdaptiveB   = (mfxU16)((pInParams->bAdaptiveB) ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_UNKNOWN);
            m_CodingOption2.BRefType    = (mfxU16)((pInParams->bBPyramid)  ? MFX_B_REF_PYRAMID   : MFX_B_REF_OFF);

            CHECK_RANGE_LIST(pInParams->nLookaheadDS, list_lookahead_ds, "la-quality");
            m_CodingOption2.LookAheadDS = (mfxU16)pInParams->nLookaheadDS;
        }
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_7)) {
            m_CodingOption2.LookAheadDepth = (mfxU16)((pInParams->nLookaheadDepth == 0) ? pInParams->nLookaheadDepth : clamp_param_int(pInParams->nLookaheadDepth, QSV_LOOKAHEAD_DEPTH_MIN, QSV_LOOKAHEAD_DEPTH_MAX, _T("la-depth")));

            CHECK_RANGE_LIST(pInParams->nTrellis, list_avc_trellis_for_options, "trellis");
            m_CodingOption2.Trellis = (mfxU16)pInParams->nTrellis;
        }
        if (pInParams->bMBBRC) {
            m_CodingOption2.MBBRC = MFX_CODINGOPTION_ON;
        }

        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_26)
            && pInParams->extBrcAdaptiveLTR) {
            m_CodingOption2.BitrateLimit = MFX_CODINGOPTION_OFF;
        }
        if (pInParams->extBRC) {
            m_CodingOption2.ExtBRC = MFX_CODINGOPTION_ON;
        }
        if (pInParams->bIntraRefresh) {
            m_CodingOption2.IntRefType = 1;
            m_CodingOption2.IntRefCycleSize = (mfxU16)((pInParams->nGOPLength >= 2) ? pInParams->nGOPLength : ((m_encFps.n() + m_encFps.d() - 1) / m_encFps.d()) * 10);
        }
        if (pInParams->bNoDeblock) {
            m_CodingOption2.DisableDeblockingIdc = MFX_CODINGOPTION_ON;
        }
        for (int i = 0; i < 3; i++) {
            pInParams->nQPMin[i] = clamp_param_int(pInParams->nQPMin[i], 0, codecMaxQP, _T("qp min"));
            pInParams->nQPMax[i] = clamp_param_int(pInParams->nQPMax[i], 0, codecMaxQP, _T("qp max"));
            const int qpMin = (std::min)(pInParams->nQPMin[i], pInParams->nQPMax[i]);
            const int qpMax = (std::max)(pInParams->nQPMin[i], pInParams->nQPMax[i]);
            pInParams->nQPMin[i] = (0 == pInParams->nQPMin[i]) ? 0 : qpMin;
            pInParams->nQPMax[i] = (0 == pInParams->nQPMax[i]) ? 0 : qpMax;
        }
        m_CodingOption2.MaxQPI = (mfxU8)pInParams->nQPMax[0];
        m_CodingOption2.MaxQPP = (mfxU8)pInParams->nQPMax[1];
        m_CodingOption2.MaxQPB = (mfxU8)pInParams->nQPMax[2];
        m_CodingOption2.MinQPI = (mfxU8)pInParams->nQPMin[0];
        m_CodingOption2.MinQPP = (mfxU8)pInParams->nQPMin[1];
        m_CodingOption2.MinQPB = (mfxU8)pInParams->nQPMin[2];
        m_EncExtParams.push_back((mfxExtBuffer *)&m_CodingOption2);
    }

    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_8)) {
        if (m_mfxEncParams.mfx.CodecId == MFX_CODEC_HEVC) {
            if (pInParams->hevc_tier != 0) {
                m_mfxEncParams.mfx.CodecLevel |= (mfxU16)pInParams->hevc_tier;
            }
        }
    }

    //API v1.11の機能
    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_11)) {
        INIT_MFX_EXT_BUFFER(m_CodingOption3, MFX_EXTBUFF_CODING_OPTION3);
        if (MFX_RATECONTROL_QVBR == m_mfxEncParams.mfx.RateControlMethod) {
            m_CodingOption3.QVBRQuality = (mfxU16)clamp_param_int(pInParams->nQVBRQuality, 1, codecMaxQP, _T("qvbr-q"));
        }
        //WinBRCの対象のレート制御モードかどうかをチェックする
        //これを行わないとInvalid Parametersとなる場合がある
        static const auto WinBRCTargetRC = make_array<int>(MFX_RATECONTROL_VBR, MFX_RATECONTROL_LA, MFX_RATECONTROL_LA_HRD, MFX_RATECONTROL_QVBR);
        if (std::find(WinBRCTargetRC.begin(), WinBRCTargetRC.end(), pInParams->nEncMode) != WinBRCTargetRC.end()
            && pInParams->nMaxBitrate != 0
            && !pInParams->extBRC) { // extbrcはWinBRCと併用できない模様
            m_CodingOption3.WinBRCSize = (mfxU16)((0 != pInParams->nWinBRCSize) ? pInParams->nWinBRCSize : ((m_encFps.n() + m_encFps.d() - 1) / m_encFps.d()));
            m_CodingOption3.WinBRCMaxAvgKbps = (mfxU16)pInParams->nMaxBitrate;
        }

        //API v1.13の機能
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_13)) {
            m_CodingOption3.DirectBiasAdjustment       = (mfxU16)((pInParams->bDirectBiasAdjust)   ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF);
            m_CodingOption3.GlobalMotionBiasAdjustment = (mfxU16)((pInParams->bGlobalMotionAdjust) ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF);
            if (pInParams->bGlobalMotionAdjust) {
                CHECK_RANGE_LIST(pInParams->nMVCostScaling, list_mv_cost_scaling, "mv-scaling");
                m_CodingOption3.MVCostScalingFactor    = (mfxU16)pInParams->nMVCostScaling;
            }
        }
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_16)) {
            m_CodingOption3.WeightedBiPred = (mfxU16)pInParams->nWeightB;
            m_CodingOption3.WeightedPred   = (mfxU16)pInParams->nWeightP;
        }
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_17)) {
            m_CodingOption3.FadeDetection = check_coding_option((mfxU16)pInParams->nFadeDetect);
        }
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_19)) {
            if (bQPOffsetUsed) {
                m_CodingOption3.EnableQPOffset = MFX_CODINGOPTION_ON;
                memcpy(m_CodingOption3.QPOffset, pInParams->pQPOffset, sizeof(pInParams->pQPOffset));
            }
        }
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_23)) {
            m_CodingOption3.RepartitionCheckEnable = (mfxU16)pInParams->nRepartitionCheck;
        }
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_26)) {
            m_CodingOption3.ExtBrcAdaptiveLTR = (mfxU16)(pInParams->extBrcAdaptiveLTR ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_UNKNOWN);
        }
        m_EncExtParams.push_back((mfxExtBuffer *)&m_CodingOption3);
    }

    //Bluray互換出力
    if (pInParams->nBluray) {
        if (   m_mfxEncParams.mfx.RateControlMethod != MFX_RATECONTROL_CBR
            && m_mfxEncParams.mfx.RateControlMethod != MFX_RATECONTROL_VBR
            && m_mfxEncParams.mfx.RateControlMethod != MFX_RATECONTROL_LA
            && m_mfxEncParams.mfx.RateControlMethod != MFX_RATECONTROL_LA_HRD) {
                if (pInParams->nBluray == 1) {
                    PrintMes(RGY_LOG_ERROR, _T("")
                        _T("Current encode mode (%s) is not preferred for Bluray encoding,\n")
                        _T("since it cannot set Max Bitrate.\n")
                        _T("Please consider using Lookahead/VBR/CBR mode for Bluray encoding.\n"), EncmodeToStr(m_mfxEncParams.mfx.RateControlMethod));
                    return RGY_ERR_INCOMPATIBLE_VIDEO_PARAM;
                } else {
                    //pInParams->nBluray == 2 -> force Bluray
                    PrintMes(RGY_LOG_WARN, _T("")
                        _T("Current encode mode (%s) is not preferred for Bluray encoding,\n")
                        _T("since it cannot set Max Bitrate.\n")
                        _T("This output might not be able to be played on a Bluray Player.\n")
                        _T("Please consider using Lookahead/VBR/CBR mode for Bluray encoding.\n"), EncmodeToStr(m_mfxEncParams.mfx.RateControlMethod));
                }
        }
        if (   m_mfxEncParams.mfx.RateControlMethod == MFX_RATECONTROL_CBR
            || m_mfxEncParams.mfx.RateControlMethod == MFX_RATECONTROL_VBR
            || m_mfxEncParams.mfx.RateControlMethod == MFX_RATECONTROL_LA
            || m_mfxEncParams.mfx.RateControlMethod == MFX_RATECONTROL_LA_HRD) {
                m_mfxEncParams.mfx.MaxKbps    = (std::min)(m_mfxEncParams.mfx.MaxKbps, (uint16_t)40000);
                m_mfxEncParams.mfx.TargetKbps = (std::min)(m_mfxEncParams.mfx.TargetKbps, m_mfxEncParams.mfx.MaxKbps);
                if (m_mfxEncParams.mfx.BufferSizeInKB == 0) {
                    m_mfxEncParams.mfx.BufferSizeInKB = m_mfxEncParams.mfx.MaxKbps / 8;
                }
                if (m_mfxEncParams.mfx.InitialDelayInKB == 0) {
                    m_mfxEncParams.mfx.InitialDelayInKB = m_mfxEncParams.mfx.BufferSizeInKB / 2;
                }
        } else {
            m_mfxEncParams.mfx.BufferSizeInKB = 25000 / 8;
        }
        m_mfxEncParams.mfx.CodecLevel = (m_mfxEncParams.mfx.CodecLevel == 0) ? MFX_LEVEL_AVC_41 : ((std::min)(m_mfxEncParams.mfx.CodecLevel, (uint16_t)MFX_LEVEL_AVC_41));
        m_mfxEncParams.mfx.NumSlice   = (std::max)(m_mfxEncParams.mfx.NumSlice, (uint16_t)4);
        m_mfxEncParams.mfx.GopOptFlag &= (~MFX_GOP_STRICT);
        m_mfxEncParams.mfx.GopRefDist = (std::min)(m_mfxEncParams.mfx.GopRefDist, (uint16_t)(3+1));
        m_mfxEncParams.mfx.GopPicSize = (int)((std::min)(m_mfxEncParams.mfx.GopPicSize, (uint16_t)30) / m_mfxEncParams.mfx.GopRefDist) * m_mfxEncParams.mfx.GopRefDist;
        m_mfxEncParams.mfx.NumRefFrame = (std::min)(m_mfxEncParams.mfx.NumRefFrame, (uint16_t)6);
        m_CodingOption.MaxDecFrameBuffering = m_mfxEncParams.mfx.NumRefFrame;
        m_CodingOption.VuiNalHrdParameters = MFX_CODINGOPTION_ON;
        m_CodingOption.VuiVclHrdParameters = MFX_CODINGOPTION_ON;
        m_CodingOption.AUDelimiter  = MFX_CODINGOPTION_ON;
        m_CodingOption.PicTimingSEI = MFX_CODINGOPTION_ON;
        m_CodingOption.ResetRefList = MFX_CODINGOPTION_ON;
        //m_CodingOption.EndOfSequence = MFX_CODINGOPTION_ON; //hwモードでは効果なし 0x00, 0x00, 0x01, 0x0a
        //m_CodingOption.EndOfStream   = MFX_CODINGOPTION_ON; //hwモードでは効果なし 0x00, 0x00, 0x01, 0x0b
        PrintMes(RGY_LOG_DEBUG, _T("InitMfxEncParams: Adjusted param for Bluray encoding.\n"));
    }

    m_EncExtParams.push_back((mfxExtBuffer *)&m_CodingOption);

    //m_mfxEncParams.mfx.TimeStampCalc = MFX_TIMESTAMPCALC_UNKNOWN;
    //m_mfxEncParams.mfx.TimeStampCalc = (mfxU16)((pInParams->vpp.nDeinterlace == MFX_DEINTERLACE_IT) ? MFX_TIMESTAMPCALC_TELECINE : MFX_TIMESTAMPCALC_UNKNOWN);
    //m_mfxEncParams.mfx.ExtendedPicStruct = pInParams->nPicStruct;

    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_3) &&
        (m_encVUI.format    != get_cx_value(list_videoformat, _T("undef")) ||
         m_encVUI.colorprim != get_cx_value(list_colorprim, _T("undef")) ||
         m_encVUI.transfer  != get_cx_value(list_transfer, _T("undef")) ||
         m_encVUI.matrix    != get_cx_value(list_colormatrix, _T("undef")) ||
         m_encVUI.colorrange == RGY_COLORRANGE_FULL
        ) ) {
#define GET_COLOR_PRM(v, list) (mfxU16)((v == COLOR_VALUE_AUTO) ? ((pInParams->input.dstHeight >= HD_HEIGHT_THRESHOLD) ? list[HD_INDEX].value : list[SD_INDEX].value) : v)
            //色設定 (for API v1.3)
            CHECK_RANGE_LIST(m_encVUI.format,    list_videoformat, "videoformat");
            CHECK_RANGE_LIST(m_encVUI.colorprim, list_colorprim,   "colorprim");
            CHECK_RANGE_LIST(m_encVUI.transfer,  list_transfer,    "transfer");
            CHECK_RANGE_LIST(m_encVUI.matrix,    list_colormatrix, "colormatrix");

            INIT_MFX_EXT_BUFFER(m_VideoSignalInfo, MFX_EXTBUFF_VIDEO_SIGNAL_INFO);
            m_VideoSignalInfo.ColourDescriptionPresent = 1; //"1"と設定しないと正しく反映されない
            m_VideoSignalInfo.VideoFormat              = (mfxU16)m_encVUI.format;
            m_VideoSignalInfo.VideoFullRange           = m_encVUI.colorrange == RGY_COLORRANGE_FULL;
            m_VideoSignalInfo.ColourPrimaries          = (mfxU16)m_encVUI.colorprim;
            m_VideoSignalInfo.TransferCharacteristics  = (mfxU16)m_encVUI.transfer;
            m_VideoSignalInfo.MatrixCoefficients       = (mfxU16)m_encVUI.matrix;
#undef GET_COLOR_PRM
            m_EncExtParams.push_back((mfxExtBuffer *)&m_VideoSignalInfo);
    }
    if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_13)
        && m_encVUI.chromaloc != RGY_CHROMALOC_UNSPECIFIED) {
        INIT_MFX_EXT_BUFFER(m_chromalocInfo, MFX_EXTBUFF_CHROMA_LOC_INFO);
        m_chromalocInfo.ChromaLocInfoPresentFlag = 1;
        m_chromalocInfo.ChromaSampleLocTypeTopField = (mfxU16)(m_encVUI.chromaloc-1);
        m_chromalocInfo.ChromaSampleLocTypeBottomField = (mfxU16)(m_encVUI.chromaloc-1);
        ////HWエンコーダではこれはサポートされていない模様なので無効化する
        //m_EncExtParams.push_back((mfxExtBuffer *)&m_chromalocInfo);
    }

    const int encBitdepth = getEncoderBitdepth(pInParams);
    const auto encCsp = getEncoderCsp(pInParams);
    m_mfxEncParams.mfx.FrameInfo.FourCC = csp_rgy_to_enc(encCsp);
    m_mfxEncParams.mfx.FrameInfo.BitDepthLuma = (mfxU16)encBitdepth;
    m_mfxEncParams.mfx.FrameInfo.BitDepthChroma = (mfxU16)encBitdepth;
    m_mfxEncParams.mfx.FrameInfo.ChromaFormat = (mfxU16)chromafmt_rgy_to_enc(RGY_CSP_CHROMA_FORMAT[encCsp]);
    m_mfxEncParams.mfx.FrameInfo.Shift = (cspShiftUsed(encCsp) && RGY_CSP_BIT_DEPTH[encCsp] - encBitdepth > 0) ? 1 : 0;
    m_mfxEncParams.mfx.FrameInfo.Width  = (mfxU16)ALIGN(m_encWidth, blocksz);
    m_mfxEncParams.mfx.FrameInfo.Height = (mfxU16)ALIGN(m_encHeight, blocksz * ((MFX_PICSTRUCT_PROGRESSIVE == m_mfxEncParams.mfx.FrameInfo.PicStruct) ? 1:2));

    m_mfxEncParams.mfx.FrameInfo.CropX = 0;
    m_mfxEncParams.mfx.FrameInfo.CropY = 0;
    m_mfxEncParams.mfx.FrameInfo.CropW = (mfxU16)m_encWidth;
    m_mfxEncParams.mfx.FrameInfo.CropH = (mfxU16)m_encHeight;

    if (m_mfxEncParams.mfx.FrameInfo.ChromaFormat == MFX_CHROMAFORMAT_YUV444) {
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_15)) {
            if (!pInParams->bUseFixedFunc) {
                PrintMes(RGY_LOG_WARN, _T("Switched to fixed function (FF) mode, as encoding in YUV444 requires FF mode.\n"));
                m_mfxEncParams.mfx.LowPower = (mfxU16)MFX_CODINGOPTION_ON;
            }
        } else {
            PrintMes(RGY_LOG_ERROR, _T("Encoding in YUV444 is not supported on this platform.\n"));
            return RGY_ERR_UNSUPPORTED;
        }
    }

    // In case of HEVC when height and/or width divided with 8 but not divided with 16
    // add extended parameter to increase performance
    if ( ( !((m_mfxEncParams.mfx.FrameInfo.CropW & 15 ) ^ 8 ) ||
           !((m_mfxEncParams.mfx.FrameInfo.CropH & 15 ) ^ 8 ) ) &&
             (m_mfxEncParams.mfx.CodecId == MFX_CODEC_HEVC) ) {
        INIT_MFX_EXT_BUFFER(m_ExtHEVCParam, MFX_EXTBUFF_HEVC_PARAM);
        m_ExtHEVCParam.PicWidthInLumaSamples = m_mfxEncParams.mfx.FrameInfo.CropW;
        m_ExtHEVCParam.PicHeightInLumaSamples = m_mfxEncParams.mfx.FrameInfo.CropH;
        m_EncExtParams.push_back((mfxExtBuffer*)&m_ExtHEVCParam);
    }

    if (m_mfxEncParams.mfx.CodecId == MFX_CODEC_VP8) {
        INIT_MFX_EXT_BUFFER(m_ExtVP8CodingOption, MFX_EXTBUFF_VP8_CODING_OPTION);
        m_ExtVP8CodingOption.SharpnessLevel = (mfxU16)clamp_param_int(pInParams->nVP8Sharpness, 0, 8, _T("sharpness"));
        m_EncExtParams.push_back((mfxExtBuffer*)&m_ExtVP8CodingOption);
    }

    if (!m_EncExtParams.empty()) {
        m_mfxEncParams.ExtParam = &m_EncExtParams[0];
        m_mfxEncParams.NumExtParam = (mfxU16)m_EncExtParams.size();
        for (const auto& extParam : m_EncExtParams) {
            PrintMes(RGY_LOG_DEBUG, _T("InitMfxEncParams: set ext param %s.\n"), fourccToStr(extParam->BufferId).c_str());
        }
    }

    PrintMes(RGY_LOG_DEBUG, _T("InitMfxEncParams: enc input frame %dx%d (%d,%d,%d,%d)\n"),
        m_mfxEncParams.mfx.FrameInfo.Width, m_mfxEncParams.mfx.FrameInfo.Height,
        m_mfxEncParams.mfx.FrameInfo.CropX, m_mfxEncParams.mfx.FrameInfo.CropY, m_mfxEncParams.mfx.FrameInfo.CropW, m_mfxEncParams.mfx.FrameInfo.CropH);
    PrintMes(RGY_LOG_DEBUG, _T("InitMfxEncParams: enc input color format %s, chroma %s, bitdepth %d, shift %d, picstruct %s\n"),
        ColorFormatToStr(m_mfxEncParams.mfx.FrameInfo.FourCC), ChromaFormatToStr(m_mfxEncParams.mfx.FrameInfo.ChromaFormat),
        m_mfxEncParams.mfx.FrameInfo.BitDepthLuma, m_mfxEncParams.mfx.FrameInfo.Shift, MFXPicStructToStr(m_mfxEncParams.mfx.FrameInfo.PicStruct).c_str());
    PrintMes(RGY_LOG_DEBUG, _T("InitMfxEncParams: set all enc params.\n"));

    m_pmfxENC.reset(new MFXVideoENCODE(m_mfxSession));
    if (!m_pmfxENC) {
        return RGY_ERR_MEMORY_ALLOC;
    }
    return RGY_ERR_NONE;
}

bool CQSVPipeline::CPUGenOpenCLSupported(const QSV_CPU_GEN cpu_gen) {
    //SandyBridgeではOpenCLフィルタをサポートしない
    return cpu_gen != CPU_GEN_SANDYBRIDGE;
}

RGY_ERR CQSVPipeline::InitOpenCL(const bool enableOpenCL) {
    if (!enableOpenCL) {
        PrintMes(RGY_LOG_DEBUG, _T("OpenCL disabled.\n"));
        return RGY_ERR_NONE;
    }
    const auto cpu_gen = getCPUGen(&m_mfxSession);
    if (!CPUGenOpenCLSupported(getCPUGen(&m_mfxSession))) {
        PrintMes(RGY_LOG_DEBUG, _T("Skip OpenCL init as OpenCL is not supported in %s platform.\n"), CPU_GEN_STR[getCPUGen(&m_mfxSession)]);
        return RGY_ERR_NONE;
    }
    const mfxHandleType hdl_t = mfxHandleTypeFromMemType(m_memType, true);
    mfxHDL hdl = nullptr;
    if (hdl_t) {
        auto sts = err_to_rgy(m_hwdev->GetHandle((hdl_t == MFX_HANDLE_DIRECT3D_DEVICE_MANAGER9) ? (mfxHandleType)0 : hdl_t, &hdl));
        RGY_ERR(sts, _T("Failed to get HW device handle."));
        PrintMes(RGY_LOG_DEBUG, _T("Got HW device handle: %p.\n"), hdl);
    }

    RGYOpenCL cl(m_pQSVLog);
    if (!RGYOpenCL::openCLloaded()) {
        PrintMes(RGY_LOG_WARN, _T("Skip OpenCL init as OpenCL is not supported on this platform.\n"));
        return RGY_ERR_NONE;
    }
    auto platforms = cl.getPlatforms("Intel");
    if (platforms.size() == 0) {
        PrintMes(RGY_LOG_ERROR, _T("Failed to find OpenCL platforms.\n"));
        return RGY_ERR_DEVICE_LOST;
    }
    PrintMes(RGY_LOG_DEBUG, _T("Created Intel OpenCL platform.\n"));

    auto& platform = platforms[0];
    if (m_memType == D3D9_MEMORY && ENABLE_RGY_OPENCL_D3D9) {
        if (platform->createDeviceListD3D9(CL_DEVICE_TYPE_GPU, (void *)hdl) != CL_SUCCESS || platform->devs().size() == 0) {
            PrintMes(RGY_LOG_ERROR, _T("Failed to find d3d9 device.\n"));
            return RGY_ERR_DEVICE_LOST;
        }
    } else if (m_memType == D3D11_MEMORY && ENABLE_RGY_OPENCL_D3D11) {
        if (platform->createDeviceListD3D11(CL_DEVICE_TYPE_GPU, (void *)hdl) != CL_SUCCESS || platform->devs().size() == 0) {
            PrintMes(RGY_LOG_ERROR, _T("Failed to find d3d11 device.\n"));
            return RGY_ERR_DEVICE_LOST;
        }
    } else if (m_memType == VA_MEMORY && ENABLE_RGY_OPENCL_VA) {
        if (platform->createDeviceListVA(CL_DEVICE_TYPE_GPU, (void *)hdl) != CL_SUCCESS || platform->devs().size() == 0) {
            PrintMes(RGY_LOG_ERROR, _T("Failed to find va device.\n"));
            return RGY_ERR_DEVICE_LOST;
        }
    } else {
        if (platform->createDeviceList(CL_DEVICE_TYPE_GPU) != CL_SUCCESS || platform->devs().size() == 0) {
            PrintMes(RGY_LOG_ERROR, _T("Failed to find gpu device.\n"));
            return RGY_ERR_DEVICE_LOST;
        }
    }
    auto devices = platform->devs();
    if ((int)devices.size() == 0) {
        PrintMes(RGY_LOG_ERROR, _T("Failed to OpenCL device.\n"));
        return RGY_ERR_DEVICE_LOST;
    }
    platform->setDev(devices[0]);

    m_cl = std::make_shared<RGYOpenCLContext>(platform, m_pQSVLog);
    if (m_cl->createContext() != CL_SUCCESS) {
        PrintMes(RGY_LOG_ERROR, _T("Failed to create OpenCL context.\n"));
        return RGY_ERR_UNKNOWN;
    }
    return RGY_ERR_NONE;
}

RGY_ERR CQSVPipeline::CreateHWDevice() {
    auto sts = RGY_ERR_NONE;

#if D3D_SURFACES_SUPPORT
    POINT point = {0, 0};
    HWND window = WindowFromPoint(point);
    m_hwdev.reset();

    if (m_memType) {
#if MFX_D3D11_SUPPORT
        if (m_memType == D3D11_MEMORY
            && (m_hwdev = std::make_unique<CQSVD3D11Device>(m_pQSVLog))) {
            m_memType = D3D11_MEMORY;
            PrintMes(RGY_LOG_DEBUG, _T("HWDevice: d3d11 - initializing...\n"));

            sts = err_to_rgy(m_hwdev->Init(NULL, 0, GetAdapterID(m_mfxSession)));
            if (sts != MFX_ERR_NONE) {
                m_hwdev.reset();
                PrintMes(RGY_LOG_DEBUG, _T("HWDevice: d3d11 - initializing failed.\n"));
            }
        }
#endif // #if MFX_D3D11_SUPPORT
        if (!m_hwdev && (m_hwdev = std::make_unique<CQSVD3D9Device>(m_pQSVLog))) {
            //もし、d3d11要求で失敗したら自動的にd3d9に切り替える
            //sessionごと切り替える必要がある
            if (m_memType != D3D9_MEMORY) {
                PrintMes(RGY_LOG_DEBUG, _T("Retry openning device, chaging to d3d9 mode, re-init session.\n"));
                InitSession(true, D3D9_MEMORY);
                m_memType = m_memType;
            }

            PrintMes(RGY_LOG_DEBUG, _T("HWDevice: d3d9 - initializing...\n"));
            sts = err_to_rgy(m_hwdev->Init(window, 0, GetAdapterID(m_mfxSession)));
        }
    }
    RGY_ERR(sts, _T("Failed to initialize HW Device."));
    PrintMes(RGY_LOG_DEBUG, _T("HWDevice: initializing device success.\n"));

#elif LIBVA_SUPPORT
    m_hwdev.reset(CreateVAAPIDevice("", MFX_LIBVA_DRM, m_pQSVLog));
    if (!m_hwdev) {
        return RGY_ERR_MEMORY_ALLOC;
    }
    sts = err_to_rgy(m_hwdev->Init(NULL, 0, GetAdapterID(m_mfxSession)));
    RGY_ERR(sts, _T("Failed to initialize HW Device."));
#endif
    return RGY_ERR_NONE;
}

RGY_ERR CQSVPipeline::ResetDevice() {
    if (m_memType & (D3D9_MEMORY | D3D11_MEMORY)) {
        PrintMes(RGY_LOG_DEBUG, _T("HWDevice: reset.\n"));
        return err_to_rgy(m_hwdev->Reset());
    }
    return RGY_ERR_NONE;
}

RGY_ERR CQSVPipeline::AllocFrames() {
    if (m_pipelineTasks.size() == 0) {
        PrintMes(RGY_LOG_ERROR, _T("allocFrames: pipeline not defined!\n"));
        return RGY_ERR_INVALID_CALL;
    }

    PrintMes(RGY_LOG_DEBUG, _T("allocFrames: m_nAsyncDepth - %d frames\n"), m_nAsyncDepth);

    PipelineTask *t0 = m_pipelineTasks[0].get();
    for (size_t ip = 1; ip < m_pipelineTasks.size(); ip++) {
        if (t0->isPassThrough()) {
            PrintMes(RGY_LOG_ERROR, _T("allocFrames: t0 cannot be path through task!\n"));
            return RGY_ERR_UNSUPPORTED;
        }
        // 次のtaskを見つける
        PipelineTask *t1 = nullptr;
        for (; ip < m_pipelineTasks.size(); ip++) {
            if (!m_pipelineTasks[ip]->isPassThrough()) { // isPassThroughがtrueなtaskはスキップ
                t1 = m_pipelineTasks[ip].get();
                break;
            }
        }
        if (t1 == nullptr) {
            PrintMes(RGY_LOG_ERROR, _T("AllocFrames: invalid pipeline, t1 not found!\n"));
            return RGY_ERR_UNSUPPORTED;
        }
        PrintMes(RGY_LOG_DEBUG, _T("AllocFrames: %s-%s\n"), t0->print().c_str(), t1->print().c_str());

        const auto t0Alloc = t0->requiredSurfOut();
        const auto t1Alloc = t1->requiredSurfIn();
        int t0RequestNumFrame = 0;
        int t1RequestNumFrame = 0;
        mfxFrameAllocRequest allocRequest = { 0 };
        bool allocateOpenCLFrame = false;
        if (t0Alloc.has_value() && t1Alloc.has_value()) {
            t0RequestNumFrame = t0Alloc.value().NumFrameSuggested;
            t1RequestNumFrame = t1Alloc.value().NumFrameSuggested;
            allocRequest = (t0->workSurfacesAllocPriority() >= t1->workSurfacesAllocPriority()) ? t0Alloc.value() : t1Alloc.value();
            allocRequest.Info.Width = std::max(t0Alloc.value().Info.Width, t1Alloc.value().Info.Width);
            allocRequest.Info.Height = std::max(t0Alloc.value().Info.Height, t1Alloc.value().Info.Height);
        } else if (t0Alloc.has_value()) {
            allocRequest = t0Alloc.value();
            t0RequestNumFrame = t0Alloc.value().NumFrameSuggested;
        } else if (t1Alloc.has_value()) {
            allocRequest = t1Alloc.value();
            t1RequestNumFrame = t1Alloc.value().NumFrameSuggested;
        } else if (t0->getOutputFrameInfo(allocRequest.Info) == RGY_ERR_NONE) {
            t0RequestNumFrame = std::max(t0->outputMaxQueueSize(), 1);
            t1RequestNumFrame = 1;
            if (   t0->taskType() == PipelineTaskType::OPENCL // openclとraw出力がつながっているような場合
                || t1->taskType() == PipelineTaskType::OPENCL // inputとopenclがつながっているような場合
            ) {
                if (!m_cl) {
                    PrintMes(RGY_LOG_ERROR, _T("AllocFrames: OpenCL filter not enabled.\n"));
                    return RGY_ERR_UNSUPPORTED;
                }
                allocateOpenCLFrame = true; // inputとopenclがつながっているような場合
            }
            if (t0->taskType() == PipelineTaskType::OPENCL) {
                t0RequestNumFrame += 4; // 内部でフレームが増える場合に備えて
            }
        } else {
            PrintMes(RGY_LOG_ERROR, _T("AllocFrames: invalid pipeline: cannot get request from either t0 or t1!\n"));
            return RGY_ERR_UNSUPPORTED;
        }
        const int requestNumFrames = std::max(1, t0RequestNumFrame + t1RequestNumFrame + m_nAsyncDepth + 1);
        if (allocateOpenCLFrame) { // OpenCLフレームを介してやり取りする場合
            const RGYFrameInfo frame(allocRequest.Info.CropW, allocRequest.Info.CropH,
                csp_enc_to_rgy(allocRequest.Info.FourCC),
                (allocRequest.Info.BitDepthLuma > 0) ? allocRequest.Info.BitDepthLuma : 8,
                picstruct_enc_to_rgy(allocRequest.Info.PicStruct));
            PrintMes(RGY_LOG_DEBUG, _T("AllocFrames: %s-%s, type: CL, %s %dx%d, request %d frames\n"),
                t0->print().c_str(), t1->print().c_str(), RGY_CSP_NAMES[frame.csp],
                frame.width, frame.height, requestNumFrames);
            auto sts = t0->workSurfacesAllocCL(requestNumFrames, frame, m_cl.get());
            if (sts != RGY_ERR_NONE) {
                PrintMes(RGY_LOG_ERROR, _T("AllocFrames:   Failed to allocate frames for %s-%s: %s."), t0->print().c_str(), t1->print().c_str(), get_err_mes(sts));
                return sts;
            }
        } else {
            switch (t0->taskType()) {
            case PipelineTaskType::MFXDEC:    allocRequest.Type |= MFX_MEMTYPE_FROM_DECODE; break;
            case PipelineTaskType::MFXVPP:    allocRequest.Type |= MFX_MEMTYPE_FROM_VPPOUT; break;
            case PipelineTaskType::OPENCL:    allocRequest.Type |= MFX_MEMTYPE_FROM_VPPOUT; break;
            case PipelineTaskType::MFXENC:    allocRequest.Type |= MFX_MEMTYPE_FROM_ENC;    break;
            case PipelineTaskType::MFXENCODE: allocRequest.Type |= MFX_MEMTYPE_FROM_ENCODE; break;
            default: break;
            }
            switch (t1->taskType()) {
            case PipelineTaskType::MFXDEC:    allocRequest.Type |= MFX_MEMTYPE_FROM_DECODE; break;
            case PipelineTaskType::MFXVPP:    allocRequest.Type |= MFX_MEMTYPE_FROM_VPPIN;  break;
            case PipelineTaskType::OPENCL:    allocRequest.Type |= MFX_MEMTYPE_FROM_VPPIN;  break;
            case PipelineTaskType::MFXENC:    allocRequest.Type |= MFX_MEMTYPE_FROM_ENC;    break;
            case PipelineTaskType::MFXENCODE: allocRequest.Type |= MFX_MEMTYPE_FROM_ENCODE; break;
            default: break;
            }

            allocRequest.AllocId = (m_bExternalAlloc) ? m_pMFXAllocator->getExtAllocCounts() : 0u;
            allocRequest.NumFrameSuggested = (mfxU16)requestNumFrames;
            allocRequest.NumFrameMin = allocRequest.NumFrameSuggested;
            PrintMes(RGY_LOG_DEBUG, _T("AllocFrames: Id: %d, %s-%s, type: %s, %s %dx%d [%d,%d,%d,%d], request %d frames\n"),
                allocRequest.AllocId, t0->print().c_str(), t1->print().c_str(), qsv_memtype_str(allocRequest.Type).c_str(), ColorFormatToStr(allocRequest.Info.FourCC),
                allocRequest.Info.Width, allocRequest.Info.Height, allocRequest.Info.CropX, allocRequest.Info.CropY, allocRequest.Info.CropW, allocRequest.Info.CropH,
                allocRequest.NumFrameSuggested);

            auto sts = t0->workSurfacesAlloc(allocRequest, m_bExternalAlloc, m_pMFXAllocator.get());
            if (sts != RGY_ERR_NONE) {
                PrintMes(RGY_LOG_ERROR, _T("AllocFrames:   Failed to allocate frames for %s-%s: %s."), t0->print().c_str(), t1->print().c_str(), get_err_mes(sts));
                return sts;
            }
        }
        t0 = t1;
    }
    return RGY_ERR_NONE;
}

RGY_ERR CQSVPipeline::CreateAllocator() {
    auto sts = RGY_ERR_NONE;
    PrintMes(RGY_LOG_DEBUG, _T("CreateAllocator: MemType: %s\n"), MemTypeToStr(m_memType));

    if (D3D9_MEMORY == m_memType || D3D11_MEMORY == m_memType || VA_MEMORY == m_memType || HW_MEMORY == m_memType) {
        sts = CreateHWDevice();
        RGY_ERR(sts, _T("Failed to CreateHWDevice."));
        PrintMes(RGY_LOG_DEBUG, _T("CreateAllocator: CreateHWDevice success.\n"));
    }
    sts = CreateAllocatorImpl(m_pMFXAllocator, m_pmfxAllocatorParams, m_bExternalAlloc, m_memType, m_hwdev.get(), m_mfxSession, m_pQSVLog);
    RGY_ERR(sts, _T("Failed to CreateAllocator."));
    PrintMes(RGY_LOG_DEBUG, _T("CreateAllocator: CreateAllocatorImpl success.\n"));
    return RGY_ERR_NONE;
}

void CQSVPipeline::DeleteHWDevice() {
    m_hwdev.reset();
}

void CQSVPipeline::DeleteAllocator() {
    m_pMFXAllocator.reset();
    m_pmfxAllocatorParams.reset();

    DeleteHWDevice();
}

CQSVPipeline::CQSVPipeline() :
    m_mfxVer({ 0 }),
    m_pStatus(),
    m_pPerfMonitor(),
    m_encWidth(0),
    m_encHeight(0),
    m_encPicstruct(RGY_PICSTRUCT_UNKNOWN),
    m_inputFps(),
    m_encFps(),
    m_outputTimebase(),
    m_encVUI(),
    m_bTimerPeriodTuning(false),
    m_pFileWriterListAudio(),
    m_pFileWriter(),
    m_AudioReaders(),
    m_pFileReader(),
    m_nAsyncDepth(0),
    m_nAVSyncMode(RGY_AVSYNC_ASSUME_CFR),
    m_InitParam(),
    m_pInitParamExtBuf(),
    m_ThreadsParam(),
    m_VideoSignalInfo(),
    m_chromalocInfo(),
    m_CodingOption(),
    m_CodingOption2(),
    m_CodingOption3(),
    m_ExtVP8CodingOption(),
    m_ExtHEVCParam(),
    m_mfxSession(),
    m_mfxDEC(),
    m_pmfxENC(),
    m_mfxVPP(),
    m_trimParam(),
    m_mfxEncParams(),
    m_prmSetIn(),
    m_EncExtParams(),
#if ENABLE_AVSW_READER
    m_Chapters(),
#endif
    m_timecode(),
    m_HDRSei(),
    m_pMFXAllocator(),
    m_pmfxAllocatorParams(),
    m_nMFXThreads(-1),
    m_memType(SYSTEM_MEMORY),
    m_bExternalAlloc(false),
    m_nProcSpeedLimit(0),
    m_pAbortByUser(nullptr),
    m_heAbort(),
    m_DecInputBitstream(),
    m_cl(),
    m_vpFilters(),
    m_videoQualityMetric(),
    m_hwdev(),
    m_pipelineTasks() {
    m_trimParam.offset = 0;

    for (size_t i = 0; i < _countof(m_pInitParamExtBuf); i++) {
        m_pInitParamExtBuf[i] = nullptr;
    }

#if ENABLE_MVC_ENCODING
    m_bIsMVC = false;
    m_MVCflags = MVC_DISABLED;
    m_nNumView = 0;
    RGY_MEMSET_ZERO(m_MVCSeqDesc);
    m_MVCSeqDesc.Header.BufferId = MFX_EXTBUFF_MVC_SEQ_DESC;
    m_MVCSeqDesc.Header.BufferSz = sizeof(m_MVCSeqDesc);
#endif
    RGY_MEMSET_ZERO(m_InitParam);
    INIT_MFX_EXT_BUFFER(m_VideoSignalInfo,    MFX_EXTBUFF_VIDEO_SIGNAL_INFO);
    INIT_MFX_EXT_BUFFER(m_chromalocInfo,      MFX_EXTBUFF_CHROMA_LOC_INFO);
    INIT_MFX_EXT_BUFFER(m_CodingOption,       MFX_EXTBUFF_CODING_OPTION);
    INIT_MFX_EXT_BUFFER(m_CodingOption2,      MFX_EXTBUFF_CODING_OPTION2);
    INIT_MFX_EXT_BUFFER(m_CodingOption3,      MFX_EXTBUFF_CODING_OPTION3);
    INIT_MFX_EXT_BUFFER(m_ExtVP8CodingOption, MFX_EXTBUFF_VP8_CODING_OPTION);
    INIT_MFX_EXT_BUFFER(m_ExtHEVCParam,       MFX_EXTBUFF_HEVC_PARAM);
    INIT_MFX_EXT_BUFFER(m_ThreadsParam,       MFX_EXTBUFF_THREADS_PARAM);

    RGY_MEMSET_ZERO(m_DecInputBitstream);

    RGY_MEMSET_ZERO(m_mfxEncParams);
}

CQSVPipeline::~CQSVPipeline() {
    Close();
}

void CQSVPipeline::SetAbortFlagPointer(bool *abortFlag) {
    m_pAbortByUser = abortFlag;
}

RGY_ERR CQSVPipeline::readChapterFile(tstring chapfile) {
#if ENABLE_AVSW_READER
    ChapterRW chapter;
    auto err = chapter.read_file(chapfile.c_str(), CODE_PAGE_UNSET, 0.0);
    if (err != AUO_CHAP_ERR_NONE) {
        PrintMes(RGY_LOG_ERROR, _T("failed to %s chapter file: \"%s\".\n"), (err == AUO_CHAP_ERR_FILE_OPEN) ? _T("open") : _T("read"), chapfile.c_str());
        return RGY_ERR_UNKNOWN;
    }
    if (chapter.chapterlist().size() == 0) {
        PrintMes(RGY_LOG_ERROR, _T("no chapter found from chapter file: \"%s\".\n"), chapfile.c_str());
        return RGY_ERR_UNKNOWN;
    }
    m_Chapters.clear();
    const auto& chapter_list = chapter.chapterlist();
    tstring chap_log;
    for (size_t i = 0; i < chapter_list.size(); i++) {
        unique_ptr<AVChapter> avchap(new AVChapter);
        avchap->time_base = av_make_q(1, 1000);
        avchap->start = chapter_list[i]->get_ms();
        avchap->end = (i < chapter_list.size()-1) ? chapter_list[i+1]->get_ms() : avchap->start + 1;
        avchap->id = (int)m_Chapters.size();
        avchap->metadata = nullptr;
        av_dict_set(&avchap->metadata, "title", chapter_list[i]->name.c_str(), 0); //chapter_list[i]->nameはUTF-8になっている
        chap_log += strsprintf(_T("chapter #%02d [%d.%02d.%02d.%03d]: %s.\n"),
            avchap->id, chapter_list[i]->h, chapter_list[i]->m, chapter_list[i]->s, chapter_list[i]->ms,
            char_to_tstring(chapter_list[i]->name, CODE_PAGE_UTF8).c_str()); //chapter_list[i]->nameはUTF-8になっている
        m_Chapters.push_back(std::move(avchap));
    }
    PrintMes(RGY_LOG_DEBUG, _T("%s"), chap_log.c_str());
    return RGY_ERR_NONE;
#else
    PrintMes(RGY_LOG_ERROR, _T("chater reading unsupported in this build"));
    return RGY_ERR_UNKNOWN;
#endif //#if ENABLE_AVSW_READER
}

RGY_ERR CQSVPipeline::InitChapters(const sInputParams *inputParam) {
#if ENABLE_AVSW_READER
    m_Chapters.clear();
    if (inputParam->common.chapterFile.length() > 0) {
        //チャプターファイルを読み込む
        auto chap_sts = readChapterFile(inputParam->common.chapterFile);
        if (chap_sts != RGY_ERR_NONE) {
            return chap_sts;
        }
    }
    if (m_Chapters.size() == 0) {
        auto pAVCodecReader = std::dynamic_pointer_cast<RGYInputAvcodec>(m_pFileReader);
        if (pAVCodecReader != nullptr) {
            auto chapterList = pAVCodecReader->GetChapterList();
            //入力ファイルのチャプターをコピーする
            for (uint32_t i = 0; i < chapterList.size(); i++) {
                unique_ptr<AVChapter> avchap(new AVChapter);
                *avchap = *chapterList[i];
                m_Chapters.push_back(std::move(avchap));
            }
        }
    }
    if (m_Chapters.size() > 0) {
        //if (inputParam->common.keyOnChapter && m_trimParam.list.size() > 0) {
        //    PrintMes(RGY_LOG_WARN, _T("--key-on-chap not supported when using --trim.\n"));
        //} else {
        //    m_keyOnChapter = inputParam->common.keyOnChapter;
        //}
    }
#endif //#if ENABLE_AVSW_READER
    return RGY_ERR_NONE;
}

RGY_CSP CQSVPipeline::getEncoderCsp(const sInputParams *pParams, int *pShift) const {
    auto csp = getMFXCsp(pParams->outputCsp, getEncoderBitdepth(pParams));
    if (pShift && fourccShiftUsed(csp_rgy_to_enc(csp))) {
        *pShift = (getEncoderBitdepth(pParams) > 8) ? 16 - pParams->outputDepth : 0;
    }
    return csp;
}

RGY_ERR CQSVPipeline::InitOutput(sInputParams *inputParams) {
    auto [err, outFrameInfo] = GetOutputVideoInfo();
    if (err != RGY_ERR_NONE) {
        PrintMes(RGY_LOG_ERROR, _T("Failed to get output frame info!\n"));
        return err;
    }
    if (!m_pmfxENC) {
        outFrameInfo->videoPrm.mfx.CodecId = 0; //エンコードしない場合は出力コーデックはraw(=0)
    }
    const auto outputVideoInfo = (outFrameInfo->isVppParam) ? videooutputinfo(outFrameInfo->videoPrmVpp.vpp.Out) : videooutputinfo(outFrameInfo->videoPrm.mfx, m_VideoSignalInfo, m_chromalocInfo);
    if (outputVideoInfo.codec == RGY_CODEC_UNKNOWN) {
        inputParams->common.AVMuxTarget &= ~RGY_MUX_VIDEO;
    }
    m_HDRSei = createHEVCHDRSei(inputParams->common.maxCll, inputParams->common.masterDisplay, inputParams->common.atcSei, m_pFileReader.get());
    if (!m_HDRSei) {
        PrintMes(RGY_LOG_ERROR, _T("Failed to parse HEVC HDR10 metadata.\n"));
        return RGY_ERR_UNSUPPORTED;
    }

    err = initWriters(m_pFileWriter, m_pFileWriterListAudio, m_pFileReader, m_AudioReaders,
        &inputParams->common, &inputParams->input, &inputParams->ctrl, outputVideoInfo,
        m_trimParam, m_outputTimebase,
#if ENABLE_AVSW_READER
        m_Chapters,
#endif //#if ENABLE_AVSW_READER
        m_HDRSei.get(),
        !check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_6),
        inputParams->bBenchmark,
        m_pStatus, m_pPerfMonitor, m_pQSVLog);
    if (err != RGY_ERR_NONE) {
        PrintMes(RGY_LOG_ERROR, _T("failed to initialize file reader(s).\n"));
        return err;
    }
    if (inputParams->common.timecode) {
        m_timecode = std::make_unique<RGYTimecode>();
        const auto tcfilename = (inputParams->common.timecodeFile.length() > 0) ? inputParams->common.timecodeFile : PathRemoveExtensionS(inputParams->common.outputFilename) + _T(".timecode.txt");
        err = m_timecode->init(tcfilename);
        if (err != RGY_ERR_NONE) {
            PrintMes(RGY_LOG_ERROR, _T("failed to open timecode file: \"%s\".\n"), tcfilename.c_str());
            return err;
        }
    }
    return RGY_ERR_NONE;
}

RGY_ERR CQSVPipeline::InitInput(sInputParams *inputParam) {
#if ENABLE_RAW_READER
#if ENABLE_AVSW_READER
    DeviceCodecCsp HWDecCodecCsp;
    HWDecCodecCsp.push_back(std::make_pair(0, getHWDecCodecCsp(m_pQSVLog, inputParam->ctrl.skipHWDecodeCheck)));
#endif
    m_pStatus.reset(new EncodeStatus());

    int subburnTrackId = 0;
    for (const auto &subburn : inputParam->vpp.subburn) {
        if (subburn.trackId > 0) {
            subburnTrackId = subburn.trackId;
            break;
        }
    }

    //--input-cspの値 (raw読み込み用の入力色空間)
    //この後上書きするので、ここで保存する
    const auto inputCspOfRawReader = inputParam->input.csp;

    //入力モジュールが、エンコーダに返すべき色空間をセット
    inputParam->input.csp = getEncoderCsp(inputParam, &inputParam->input.bitdepth);

    auto sts = initReaders(m_pFileReader, m_AudioReaders, &inputParam->input, inputCspOfRawReader,
        m_pStatus, &inputParam->common, &inputParam->ctrl, HWDecCodecCsp, subburnTrackId,
        (ENABLE_VPP_FILTER_RFF) ? inputParam->vpp.rff : false,
        (ENABLE_VPP_FILTER_AFS) ? inputParam->vpp.afs.enable : false,
        nullptr, m_pPerfMonitor.get(), m_pQSVLog);
    if (sts != RGY_ERR_NONE) {
        PrintMes(RGY_LOG_ERROR, _T("failed to initialize file reader(s).\n"));
        return sts;
    }
    PrintMes(RGY_LOG_DEBUG, _T("initReaders: Success.\n"));

    m_inputFps = rgy_rational<int>(inputParam->input.fpsN, inputParam->input.fpsD);
    m_outputTimebase = m_inputFps.inv() * rgy_rational<int>(1, 4);
    if (m_nAVSyncMode & RGY_AVSYNC_VFR) {
        //avsync vfr時は、入力streamのtimebaseをそのまま使用する
        m_outputTimebase = m_pFileReader->getInputTimebase();
    }

    if (
#if ENABLE_AVSW_READER
        std::dynamic_pointer_cast<RGYInputAvcodec>(m_pFileReader) == nullptr &&
#endif
        inputParam->common.pTrimList && inputParam->common.nTrimCount > 0) {
        //avhw/avswリーダー以外は、trimは自分ではセットされないので、ここでセットする
        sTrimParam trimParam;
        trimParam.list = make_vector(inputParam->common.pTrimList, inputParam->common.nTrimCount);
        trimParam.offset = 0;
        m_pFileReader->SetTrimParam(trimParam);
    }
    //trim情報をリーダーから取得する
    m_trimParam = m_pFileReader->GetTrimParam();
    if (m_trimParam.list.size() > 0) {
        PrintMes(RGY_LOG_DEBUG, _T("Input: trim options\n"));
        for (int i = 0; i < (int)m_trimParam.list.size(); i++) {
            PrintMes(RGY_LOG_DEBUG, _T("%d-%d "), m_trimParam.list[i].start, m_trimParam.list[i].fin);
        }
        PrintMes(RGY_LOG_DEBUG, _T(" (offset: %d)\n"), m_trimParam.offset);
    }

#if ENABLE_AVSW_READER
    auto pAVCodecReader = std::dynamic_pointer_cast<RGYInputAvcodec>(m_pFileReader);
    if ((m_nAVSyncMode & (RGY_AVSYNC_VFR | RGY_AVSYNC_FORCE_CFR))
#if ENABLE_VPP_FILTER_RFF
        || inputParam->vpp.rff
#endif
        ) {
        tstring err_target;
        if (m_nAVSyncMode & RGY_AVSYNC_VFR)       err_target += _T("avsync vfr, ");
        if (m_nAVSyncMode & RGY_AVSYNC_FORCE_CFR) err_target += _T("avsync forcecfr, ");
#if ENABLE_VPP_FILTER_RFF
        if (inputParam->vpp.rff)                  err_target += _T("vpp-rff, ");
#endif
        err_target = err_target.substr(0, err_target.length()-2);

        if (pAVCodecReader) {
            //timestampになんらかの問題がある場合、vpp-rffとavsync vfrは使用できない
            const auto timestamp_status = pAVCodecReader->GetFramePosList()->getStreamPtsStatus();
            if ((timestamp_status & (~RGY_PTS_NORMAL)) != 0) {

                tstring err_sts;
                if (timestamp_status & RGY_PTS_SOMETIMES_INVALID) err_sts += _T("SOMETIMES_INVALID, "); //時折、無効なptsを得る
                if (timestamp_status & RGY_PTS_HALF_INVALID)      err_sts += _T("HALF_INVALID, "); //PAFFなため、半分のフレームのptsやdtsが無効
                if (timestamp_status & RGY_PTS_ALL_INVALID)       err_sts += _T("ALL_INVALID, "); //すべてのフレームのptsやdtsが無効
                if (timestamp_status & RGY_PTS_NONKEY_INVALID)    err_sts += _T("NONKEY_INVALID, "); //キーフレーム以外のフレームのptsやdtsが無効
                if (timestamp_status & RGY_PTS_DUPLICATE)         err_sts += _T("PTS_DUPLICATE, "); //重複するpts/dtsが存在する
                if (timestamp_status & RGY_DTS_SOMETIMES_INVALID) err_sts += _T("DTS_SOMETIMES_INVALID, "); //時折、無効なdtsを得る
                err_sts = err_sts.substr(0, err_sts.length()-2);

                PrintMes(RGY_LOG_ERROR, _T("timestamp not acquired successfully from input stream, %s cannot be used. \n  [0x%x] %s\n"),
                    err_target.c_str(), (uint32_t)timestamp_status, err_sts.c_str());
                return RGY_ERR_UNKNOWN;
            }
            PrintMes(RGY_LOG_DEBUG, _T("timestamp check: 0x%x\n"), timestamp_status);
        } else if (m_outputTimebase.n() == 0 || !m_outputTimebase.is_valid()) {
            PrintMes(RGY_LOG_ERROR, _T("%s cannot be used with current reader.\n"), err_target.c_str());
            return RGY_ERR_UNKNOWN;
        }
    } else if (pAVCodecReader && ((pAVCodecReader->GetFramePosList()->getStreamPtsStatus() & (~RGY_PTS_NORMAL)) == 0)) {
        if (!ENCODER_QSV) {
            m_nAVSyncMode |= RGY_AVSYNC_VFR;
            const auto timebaseStreamIn = to_rgy(pAVCodecReader->GetInputVideoStream()->time_base);
            if ((timebaseStreamIn.inv() * m_inputFps.inv()).d() == 1 || timebaseStreamIn.n() > 1000) { //fpsを割り切れるtimebaseなら
                if (!inputParam->vpp.afs.enable && !inputParam->vpp.rff) {
                    m_outputTimebase = m_inputFps.inv() * rgy_rational<int>(1, 8);
                }
            }
        }
        PrintMes(RGY_LOG_DEBUG, _T("vfr mode automatically enabled with timebase %d/%d\n"), m_outputTimebase.n(), m_outputTimebase.d());
    }
#if 0
    if (inputParam->common.dynamicHdr10plusJson.length() > 0) {
        m_hdr10plus = initDynamicHDR10Plus(inputParam->common.dynamicHdr10plusJson, m_pNVLog);
        if (!m_hdr10plus) {
            PrintMes(RGY_LOG_ERROR, _T("Failed to initialize hdr10plus reader.\n"));
            return RGY_ERR_UNKNOWN;
        }
    }
#endif
#endif //#if ENABLE_AVSW_READER
    return RGY_ERR_NONE;
#else
    return RGY_ERR_UNSUPPORTED;
#endif //#if ENABLE_RAW_READER
}

RGY_ERR CQSVPipeline::CheckParam(sInputParams *inputParam) {
    const auto inputFrameInfo = m_pFileReader->GetInputFrameInfo();

    //いろいろなチェックの前提となる
    applyInputVUIToColorspaceParams(inputParam);

    if ((inputParam->memType & HW_MEMORY) == HW_MEMORY) { //自動モードの場合
        //OpenCLフィルタを使う場合はd3d11を使用する
        if (preferD3D11Mode(inputParam)) {
            inputParam->memType = D3D11_MEMORY;
            PrintMes(RGY_LOG_DEBUG, _T("d3d11 mode prefered, switched to d3d11 mode.\n"));
        //出力コーデックがrawなら、systemメモリを自動的に使用する
        } else if (inputParam->CodecId == MFX_CODEC_RAW) {
            inputParam->memType = SYSTEM_MEMORY;
            PrintMes(RGY_LOG_DEBUG, _T("Automatically selecting system memory for output raw frames.\n"));
        }
    }

    if ((inputParam->memType & HW_MEMORY)
        && (inputFrameInfo.csp == RGY_CSP_NV16 || inputFrameInfo.csp == RGY_CSP_P210)) {
        PrintMes(RGY_LOG_WARN, _T("Currently yuv422 surfaces are not supported by d3d9/d3d11 memory.\n"));
        PrintMes(RGY_LOG_WARN, _T("Switching to system memory.\n"));
        inputParam->memType = SYSTEM_MEMORY;
    }

    //デコードを行う場合は、入力バッファサイズを常に1に設定する (そうしないと正常に動かない)
    //また、バッファサイズを拡大しても特に高速化しない
    if (m_pFileReader->getInputCodec() != RGY_CODEC_UNKNOWN) {
        inputParam->nInputBufSize = 1;
        //Haswell以前はHEVCデコーダを使用する場合はD3D11メモリを使用しないと正常に稼働しない (4080ドライバ)
        if (getCPUGen(&m_mfxSession) <= CPU_GEN_HASWELL && m_pFileReader->getInputCodec() == RGY_CODEC_HEVC) {
            if (inputParam->memType & D3D9_MEMORY) {
                inputParam->memType &= ~D3D9_MEMORY;
                inputParam->memType |= D3D11_MEMORY;
            }
            PrintMes(RGY_LOG_DEBUG, _T("Switched to d3d11 mode for HEVC decoding on Haswell.\n"));
        }
        if (m_pFileReader->getInputCodec() == RGY_CODEC_AV1) {
            if (inputParam->memType & D3D9_MEMORY) {
                inputParam->memType &= ~D3D9_MEMORY;
                inputParam->memType |= D3D11_MEMORY;
            }
            PrintMes(RGY_LOG_DEBUG, _T("Switched to d3d11 mode for AV1 decoding.\n"));
        }
    }

    // 解像度の条件とcrop
    int h_mul = 2;
    bool output_interlaced = ((inputParam->input.picstruct & RGY_PICSTRUCT_INTERLACED) != 0 && !inputParam->vppmfx.deinterlace);
    if (output_interlaced) {
        h_mul *= 2;
    }
    // crop設定の確認
    if (inputParam->input.crop.e.left % 2 != 0 || inputParam->input.crop.e.right % 2 != 0) {
        PrintMes(RGY_LOG_ERROR, _T("crop width should be a multiple of 2.\n"));
        return RGY_ERR_INVALID_VIDEO_PARAM;
    }
    if (inputParam->input.crop.e.bottom % h_mul != 0 || inputParam->input.crop.e.up % h_mul != 0) {
        PrintMes(RGY_LOG_ERROR, _T("crop height should be a multiple of %d.\n"));
        return RGY_ERR_INVALID_VIDEO_PARAM;
    }
    if (0 == inputParam->input.srcWidth || 0 == inputParam->input.srcHeight) {
        PrintMes(RGY_LOG_ERROR, _T("--input-res must be specified with raw input.\n"));
        return RGY_ERR_INVALID_VIDEO_PARAM;
    }
    if (inputParam->input.fpsN == 0 || inputParam->input.fpsD == 0) {
        PrintMes(RGY_LOG_ERROR, _T("--fps must be specified with raw input.\n"));
        return RGY_ERR_INVALID_VIDEO_PARAM;
    }
    if (inputParam->input.srcWidth < (inputParam->input.crop.e.left + inputParam->input.crop.e.right)
        || inputParam->input.srcHeight < (inputParam->input.crop.e.bottom + inputParam->input.crop.e.up)) {
        PrintMes(RGY_LOG_ERROR, _T("crop size is too big.\n"));
        return RGY_ERR_INVALID_VIDEO_PARAM;
    }

    //解像度の自動設定
    auto outpar = std::make_pair(inputParam->nPAR[0], inputParam->nPAR[1]);
    if ((!inputParam->nPAR[0] || !inputParam->nPAR[1]) //SAR比の指定がない
        && inputParam->input.sar[0] && inputParam->input.sar[1] //入力側からSAR比を取得ずみ
        && (inputParam->input.dstWidth == inputParam->input.srcWidth && inputParam->input.dstHeight == inputParam->input.srcHeight)) {//リサイズは行われない
        outpar = std::make_pair(inputParam->input.sar[0], inputParam->input.sar[1]);
    }
    if (inputParam->input.dstWidth < 0 && inputParam->input.dstHeight < 0) {
        PrintMes(RGY_LOG_ERROR, _T("Either one of output resolution must be positive value.\n"));
        return RGY_ERR_INVALID_VIDEO_PARAM;
    }

    set_auto_resolution(inputParam->input.dstWidth, inputParam->input.dstHeight, outpar.first, outpar.second,
        inputParam->input.srcWidth, inputParam->input.srcHeight, inputParam->input.sar[0], inputParam->input.sar[1], inputParam->input.crop);

    // 解像度の条件とcrop
    if (inputParam->input.dstWidth % 2 != 0) {
        PrintMes(RGY_LOG_ERROR, _T("output width should be a multiple of 2.\n"));
        return RGY_ERR_INVALID_VIDEO_PARAM;
    }

    if (inputParam->input.dstHeight % h_mul != 0) {
        PrintMes(RGY_LOG_ERROR, _T("output height should be a multiple of %d.\n"), h_mul);
        return RGY_ERR_INVALID_VIDEO_PARAM;
    }

    //入力バッファサイズの範囲チェック
    inputParam->nInputBufSize = (mfxU16)clamp_param_int(inputParam->nInputBufSize, QSV_INPUT_BUF_MIN, QSV_INPUT_BUF_MAX, _T("input-buf"));

    return RGY_ERR_NONE;
}

void CQSVPipeline::applyInputVUIToColorspaceParams(sInputParams *inputParam) {
    auto currentVUI = inputParam->input.vui;
    for (size_t i = 0; i < inputParam->vpp.colorspace.convs.size(); i++) {
        auto conv_from = inputParam->vpp.colorspace.convs[i].from;
        conv_from.apply_auto(currentVUI, inputParam->input.srcHeight);
        if (i == 0) {
            inputParam->vppmfx.colorspace.from.matrix = conv_from.matrix;
            inputParam->vppmfx.colorspace.from.range = conv_from.colorrange;
        }

        auto conv_to = inputParam->vpp.colorspace.convs[i].to;
        const bool is_last_conversion = i == (inputParam->vpp.colorspace.convs.size() - 1);
        if (is_last_conversion) {
            conv_to.apply_auto(m_encVUI, m_encHeight);
            inputParam->vppmfx.colorspace.to.matrix = conv_to.matrix;
            inputParam->vppmfx.colorspace.to.range = conv_to.colorrange;
        } else {
            conv_to.apply_auto(conv_from, inputParam->input.srcHeight);
        }
    }
}

std::vector<VppType> CQSVPipeline::InitFiltersCreateVppList(const sInputParams *inputParam, const bool cspConvRequired, const bool cropRequired, const RGY_VPP_RESIZE_TYPE resizeRequired) {
    std::vector<VppType> filterPipeline;
    filterPipeline.reserve((size_t)VppType::CL_MAX);

    if (cspConvRequired || cropRequired)   filterPipeline.push_back(VppType::MFX_CROP);
    if (inputParam->vpp.colorspace.enable) {
        bool requireOpenCL = inputParam->vpp.colorspace.hdr2sdr.tonemap != HDR2SDR_DISABLED;
        if (!requireOpenCL) {
            auto currentVUI = inputParam->input.vui;
            for (size_t i = 0; i < inputParam->vpp.colorspace.convs.size(); i++) {
                auto conv_from = inputParam->vpp.colorspace.convs[i].from;
                auto conv_to = inputParam->vpp.colorspace.convs[i].to;
                if (conv_from.chromaloc != conv_to.chromaloc
                    || conv_from.colorprim != conv_to.colorprim
                    || conv_from.transfer != conv_to.transfer) {
                    requireOpenCL = true;
                } else if (conv_from.matrix != conv_to.matrix
                    && (conv_from.matrix != RGY_MATRIX_ST170_M && conv_from.matrix != RGY_MATRIX_BT709)
                    && (conv_to.matrix != RGY_MATRIX_ST170_M && conv_to.matrix != RGY_MATRIX_BT709)) {
                    requireOpenCL = true;
                }
            }
        }
        filterPipeline.push_back((requireOpenCL) ? VppType::CL_COLORSPACE : VppType::MFX_COLORSPACE);
    }
    if (inputParam->vpp.delogo.enable)     filterPipeline.push_back(VppType::CL_DELOGO);
    if (inputParam->vpp.afs.enable)        filterPipeline.push_back(VppType::CL_AFS);
    if (inputParam->vpp.nnedi.enable)      filterPipeline.push_back(VppType::CL_NNEDI);
    if (inputParam->vppmfx.deinterlace != MFX_DEINTERLACE_NONE)  filterPipeline.push_back(VppType::MFX_DEINTERLACE);
    if (inputParam->vpp.decimate.enable)   filterPipeline.push_back(VppType::CL_DECIMATE);
    if (inputParam->vpp.mpdecimate.enable) filterPipeline.push_back(VppType::CL_MPDECIMATE);
    if (inputParam->vpp.knn.enable)        filterPipeline.push_back(VppType::CL_DENOISE_KNN);
    if (inputParam->vpp.pmd.enable)        filterPipeline.push_back(VppType::CL_DENOISE_PMD);
    if (inputParam->vpp.smooth.enable)     filterPipeline.push_back(VppType::CL_DENOISE_SMOOTH);
    if (inputParam->vppmfx.denoise.enable) filterPipeline.push_back(VppType::MFX_DENOISE);
    if (inputParam->vppmfx.imageStabilizer != 0) filterPipeline.push_back(VppType::MFX_IMAGE_STABILIZATION);
    if (inputParam->vppmfx.mctf.enable)    filterPipeline.push_back(VppType::MFX_MCTF);
    if (inputParam->vpp.subburn.size()>0)  filterPipeline.push_back(VppType::CL_SUBBURN);
    if (     resizeRequired == RGY_VPP_RESIZE_TYPE_OPENCL) filterPipeline.push_back(VppType::CL_RESIZE);
    else if (resizeRequired != RGY_VPP_RESIZE_TYPE_NONE)   filterPipeline.push_back(VppType::MFX_RESIZE);
    if (inputParam->vpp.unsharp.enable)    filterPipeline.push_back(VppType::CL_UNSHARP);
    if (inputParam->vpp.edgelevel.enable)  filterPipeline.push_back(VppType::CL_EDGELEVEL);
    if (inputParam->vpp.warpsharp.enable)  filterPipeline.push_back(VppType::CL_WARPSHARP);
    if (inputParam->vppmfx.detail.enable)  filterPipeline.push_back(VppType::MFX_DETAIL_ENHANCE);
    if (inputParam->vppmfx.mirrorType != MFX_MIRRORING_DISABLED) filterPipeline.push_back(VppType::MFX_MIRROR);
    if (inputParam->vpp.transform.enable)  filterPipeline.push_back(VppType::CL_TRANSFORM);
    if (inputParam->vpp.tweak.enable)      filterPipeline.push_back(VppType::CL_TWEAK);
    if (inputParam->vpp.deband.enable)     filterPipeline.push_back(VppType::CL_DEBAND);
    if (inputParam->vpp.pad.enable)        filterPipeline.push_back(VppType::CL_PAD);

    if (filterPipeline.size() == 0) {
        return filterPipeline;
    }

    // cropとresizeはmfxとopencl両方ともあるので、前後のフィルタがどちらもOpenCLだったら、そちらに合わせる
    for (size_t i = 0; i < filterPipeline.size(); i++) {
        const VppFilterType prev = (i >= 1)                        ? getVppFilterType(filterPipeline[i - 1]) : VppFilterType::FILTER_NONE;
        const VppFilterType next = (i + 1 < filterPipeline.size()) ? getVppFilterType(filterPipeline[i + 1]) : VppFilterType::FILTER_NONE;
        if (filterPipeline[i] == VppType::MFX_RESIZE) {
            if (resizeRequired == RGY_VPP_RESIZE_TYPE_AUTO // 自動以外の指定があれば、それに従うので、自動の場合のみ変更
                && m_cl
                && prev == VppFilterType::FILTER_OPENCL
                && next == VppFilterType::FILTER_OPENCL) {
                filterPipeline[i] = VppType::CL_RESIZE; // OpenCLに挟まれていたら、OpenCLのresizeを優先する
            }
        } else if (filterPipeline[i] == VppType::MFX_CROP) {
            if (m_cl
                && (prev == VppFilterType::FILTER_OPENCL || next == VppFilterType::FILTER_OPENCL)
                && (prev != VppFilterType::FILTER_MFX    || next != VppFilterType::FILTER_MFX)) {
                filterPipeline[i] = VppType::CL_CROP; // OpenCLに挟まれていたら、OpenCLのcropを優先する
            }
        } else if (filterPipeline[i] == VppType::MFX_COLORSPACE) {
            if (m_cl
                && prev == VppFilterType::FILTER_OPENCL
                && next == VppFilterType::FILTER_OPENCL) {
                filterPipeline[i] = VppType::CL_COLORSPACE; // OpenCLに挟まれていたら、OpenCLのcolorspaceを優先する
            }
        }
    }
    return filterPipeline;
}

std::pair<RGY_ERR, std::unique_ptr<QSVVppMfx>> CQSVPipeline::AddFilterMFX(
    RGYFrameInfo& frameInfo, rgy_rational<int>& fps,
    const VppType vppType, const sVppParams *params, const RGY_CSP outCsp, const int outBitdepth, const sInputCrop *crop, const std::pair<int,int> resize, const int blockSize) {
    RGYFrameInfo frameIn = frameInfo;
    sVppParams vppParams;
    vppParams.bEnable = true;
    switch (vppType) {
    case VppType::MFX_COPY: break;
    case VppType::MFX_DEINTERLACE:         vppParams.deinterlace = params->deinterlace; break;
    case VppType::MFX_DENOISE:             vppParams.denoise = params->denoise; break;
    case VppType::MFX_DETAIL_ENHANCE:      vppParams.detail = params->detail; break;
    case VppType::MFX_COLORSPACE:          vppParams.colorspace = params->colorspace; vppParams.colorspace.enable = true; break;
    case VppType::MFX_IMAGE_STABILIZATION: vppParams.imageStabilizer = params->imageStabilizer; break;
    case VppType::MFX_ROTATE:              vppParams.rotate = params->rotate; break;
    case VppType::MFX_MIRROR:              vppParams.mirrorType = params->mirrorType; break;
    case VppType::MFX_MCTF:                vppParams.mctf = params->mctf; break;
    case VppType::MFX_RESIZE:              vppParams.bUseResize = true;
                                           vppParams.resizeInterp = params->resizeInterp;
                                           vppParams.resizeMode = params->resizeMode;
                                           frameInfo.width = resize.first;
                                           frameInfo.height = resize.second; break;
    case VppType::MFX_CROP:                frameInfo.width  -= (crop) ? (crop->e.left + crop->e.right) : 0;
                                           frameInfo.height -= (crop) ? (crop->e.up + crop->e.bottom)  : 0; break;
    case VppType::MFX_FPS_CONV:
    default:
        return { RGY_ERR_UNSUPPORTED, std::unique_ptr<QSVVppMfx>() };
    }

    frameInfo.csp = outCsp; // 常に適用
    frameInfo.bitdepth = outBitdepth;

    mfxIMPL impl;
    m_mfxSession.QueryIMPL(&impl);
    auto mfxvpp = std::make_unique<QSVVppMfx>(m_hwdev.get(), m_pMFXAllocator.get(), m_mfxVer, impl, m_memType, m_nAsyncDepth, m_pQSVLog);
    auto err = mfxvpp->SetParam(vppParams, frameInfo, frameIn, (vppType == VppType::MFX_CROP) ? crop : nullptr,
        fps, rgy_rational<int>(1,1), blockSize);
    if (err != RGY_ERR_NONE) {
        return { err, std::unique_ptr<QSVVppMfx>() };
    }

    if (vppType != VppType::MFX_COPY // copyの時は意図的にアクションがない
        && mfxvpp->GetVppList().size() == 0) {
        PrintMes(RGY_LOG_WARN, _T("filtering has no action.\n"));
        return { err, std::unique_ptr<QSVVppMfx>() };
    }

    //入力フレーム情報を更新
    frameInfo = mfxvpp->GetFrameOut();
    fps = mfxvpp->GetOutFps();

    return { RGY_ERR_NONE, std::move(mfxvpp) };
}

RGY_ERR CQSVPipeline::AddFilterOpenCL(std::vector<std::unique_ptr<RGYFilter>>& clfilters,
    RGYFrameInfo& inputFrame, rgy_rational<int>& fps, const VppType vppType, const sInputParams *params, const sInputCrop *crop, const std::pair<int, int> resize) {
    //colorspace
    if (vppType == VppType::CL_COLORSPACE) {
        unique_ptr<RGYFilterColorspace> filter(new RGYFilterColorspace(m_cl));
        shared_ptr<RGYFilterParamColorspace> param(new RGYFilterParamColorspace());
        param->colorspace = params->vpp.colorspace;
        param->encCsp = inputFrame.csp;
        param->VuiIn = params->input.vui;
        param->frameIn = inputFrame;
        param->frameOut = inputFrame;
        param->baseFps = m_encFps;
        auto sts = filter->init(param, m_pQSVLog);
        if (sts != RGY_ERR_NONE) {
            return sts;
        }
        //入力フレーム情報を更新
        inputFrame = param->frameOut;
        m_encFps = param->baseFps;
        //登録
        clfilters.push_back(std::move(filter));
        return RGY_ERR_NONE;
    }
    //delogo
    if (vppType == VppType::CL_DELOGO) {
        unique_ptr<RGYFilter> filter(new RGYFilterDelogo(m_cl));
        shared_ptr<RGYFilterParamDelogo> param(new RGYFilterParamDelogo());
        param->inputFileName = params->common.inputFilename.c_str();
        param->delogo = params->vpp.delogo;
        param->frameIn = inputFrame;
        param->frameOut = inputFrame;
        param->baseFps = m_encFps;
        param->bOutOverwrite = true;
        auto sts = filter->init(param, m_pQSVLog);
        if (sts != RGY_ERR_NONE) {
            return sts;
        }
        //入力フレーム情報を更新
        inputFrame = param->frameOut;
        m_encFps = param->baseFps;
        //登録
        clfilters.push_back(std::move(filter));
        return RGY_ERR_NONE;
    }
    //afs
    if (vppType == VppType::CL_AFS) {
        if ((params->input.picstruct & (RGY_PICSTRUCT_TFF | RGY_PICSTRUCT_BFF)) == 0) {
            PrintMes(RGY_LOG_ERROR, _T("Please set input interlace field order (--interlace tff/bff) for vpp-afs.\n"));
            return RGY_ERR_INVALID_PARAM;
        }
        unique_ptr<RGYFilter> filter(new RGYFilterAfs(m_cl));
        shared_ptr<RGYFilterParamAfs> param(new RGYFilterParamAfs());
        param->afs = params->vpp.afs;
        param->afs.tb_order = (params->input.picstruct & RGY_PICSTRUCT_TFF) != 0;
        if (params->common.timecode && param->afs.timecode) {
            param->afs.timecode = 2;
        }
        param->frameIn = inputFrame;
        param->frameOut = inputFrame;
        param->inFps = m_inputFps;
        param->inTimebase = m_outputTimebase;
        param->outTimebase = m_outputTimebase;
        param->baseFps = m_encFps;
        param->outFilename = params->common.outputFilename;
        param->bOutOverwrite = false;
        auto sts = filter->init(param, m_pQSVLog);
        if (sts != RGY_ERR_NONE) {
            return sts;
        }
        //入力フレーム情報を更新
        inputFrame = param->frameOut;
        m_encFps = param->baseFps;
        //登録
        clfilters.push_back(std::move(filter));
        return RGY_ERR_NONE;
    }
    //nnedi
    if (vppType == VppType::CL_NNEDI) {
        if ((params->input.picstruct & (RGY_PICSTRUCT_TFF | RGY_PICSTRUCT_BFF)) == 0) {
            PrintMes(RGY_LOG_ERROR, _T("Please set input interlace field order (--interlace tff/bff) for vpp-nnedi.\n"));
            return RGY_ERR_INVALID_PARAM;
        }
        unique_ptr<RGYFilter> filter(new RGYFilterNnedi(m_cl));
        shared_ptr<RGYFilterParamNnedi> param(new RGYFilterParamNnedi());
        param->nnedi = params->vpp.nnedi;
        param->frameIn = inputFrame;
        param->frameOut = inputFrame;
        param->baseFps = m_encFps;
        param->bOutOverwrite = false;
        auto sts = filter->init(param, m_pQSVLog);
        if (sts != RGY_ERR_NONE) {
            return sts;
        }
        //入力フレーム情報を更新
        inputFrame = param->frameOut;
        m_encFps = param->baseFps;
        //登録
        clfilters.push_back(std::move(filter));
        return RGY_ERR_NONE;
    }
    //decimate
    if (vppType == VppType::CL_DECIMATE) {
        unique_ptr<RGYFilter> filter(new RGYFilterDecimate(m_cl));
        shared_ptr<RGYFilterParamDecimate> param(new RGYFilterParamDecimate());
        param->decimate = params->vpp.decimate;
        //QSV:Broadwell以前の環境では、なぜか別のキューで実行しようとすると、永遠にqueueMapBufferが開始されず、フリーズしてしまう
        //こういうケースでは標準のキューを使って逐次実行する
        param->useSeparateQueue = getCPUGen(&m_mfxSession) >= CPU_GEN_SKYLAKE;
        param->frameIn = inputFrame;
        param->frameOut = inputFrame;
        param->baseFps = m_encFps;
        param->bOutOverwrite = false;
        auto sts = filter->init(param, m_pQSVLog);
        if (sts != RGY_ERR_NONE) {
            return sts;
        }
        //入力フレーム情報を更新
        inputFrame = param->frameOut;
        m_encFps = param->baseFps;
        //登録
        clfilters.push_back(std::move(filter));
        return RGY_ERR_NONE;
    }
    //mpdecimate
    if (vppType == VppType::CL_MPDECIMATE) {
        unique_ptr<RGYFilter> filter(new RGYFilterMpdecimate(m_cl));
        shared_ptr<RGYFilterParamMpdecimate> param(new RGYFilterParamMpdecimate());
        param->mpdecimate = params->vpp.mpdecimate;
        //QSV:Broadwell以前の環境では、なぜか別のキューで実行しようとすると、永遠にqueueMapBufferが開始されず、フリーズしてしまう
        //こういうケースでは標準のキューを使って逐次実行する
        param->useSeparateQueue = getCPUGen(&m_mfxSession) >= CPU_GEN_SKYLAKE;
        param->frameIn = inputFrame;
        param->frameOut = inputFrame;
        param->baseFps = m_encFps;
        param->bOutOverwrite = false;
        auto sts = filter->init(param, m_pQSVLog);
        if (sts != RGY_ERR_NONE) {
            return sts;
        }
        //入力フレーム情報を更新
        inputFrame = param->frameOut;
        m_encFps = param->baseFps;
        //登録
        clfilters.push_back(std::move(filter));
        return RGY_ERR_NONE;
    }
    //回転
    if (vppType == VppType::CL_TRANSFORM) {
        unique_ptr<RGYFilter> filter(new RGYFilterTransform(m_cl));
        shared_ptr<RGYFilterParamTransform> param(new RGYFilterParamTransform());
        param->trans = params->vpp.transform;
        param->frameIn = inputFrame;
        param->frameOut = inputFrame;
        param->baseFps = m_encFps;
        param->bOutOverwrite = false;
        auto sts = filter->init(param, m_pQSVLog);
        if (sts != RGY_ERR_NONE) {
            return sts;
        }
        //入力フレーム情報を更新
        inputFrame = param->frameOut;
        m_encFps = param->baseFps;
        //登録
        clfilters.push_back(std::move(filter));
        return RGY_ERR_NONE;
    }
    //knn
    if (vppType == VppType::CL_DENOISE_KNN) {
        unique_ptr<RGYFilter> filter(new RGYFilterDenoiseKnn(m_cl));
        shared_ptr<RGYFilterParamDenoiseKnn> param(new RGYFilterParamDenoiseKnn());
        param->knn = params->vpp.knn;
        param->frameIn = inputFrame;
        param->frameOut = inputFrame;
        param->baseFps = m_encFps;
        param->bOutOverwrite = false;
        auto sts = filter->init(param, m_pQSVLog);
        if (sts != RGY_ERR_NONE) {
            return sts;
        }
        //入力フレーム情報を更新
        inputFrame = param->frameOut;
        m_encFps = param->baseFps;
        //登録
        clfilters.push_back(std::move(filter));
        return RGY_ERR_NONE;
    }
    //pmd
    if (vppType == VppType::CL_DENOISE_PMD) {
        unique_ptr<RGYFilter> filter(new RGYFilterDenoisePmd(m_cl));
        shared_ptr<RGYFilterParamDenoisePmd> param(new RGYFilterParamDenoisePmd());
        param->pmd = params->vpp.pmd;
        param->frameIn = inputFrame;
        param->frameOut = inputFrame;
        param->baseFps = m_encFps;
        param->bOutOverwrite = false;
        auto sts = filter->init(param, m_pQSVLog);
        if (sts != RGY_ERR_NONE) {
            return sts;
        }
        //入力フレーム情報を更新
        inputFrame = param->frameOut;
        m_encFps = param->baseFps;
        //登録
        clfilters.push_back(std::move(filter));
        return RGY_ERR_NONE;
    }
    //smooth
    if (vppType == VppType::CL_DENOISE_SMOOTH) {
        unique_ptr<RGYFilter> filter(new RGYFilterSmooth(m_cl));
        shared_ptr<RGYFilterParamSmooth> param(new RGYFilterParamSmooth());
        param->smooth = params->vpp.smooth;
        param->qpTableRef = nullptr;
        param->frameIn = inputFrame;
        param->frameOut = inputFrame;
        param->baseFps = m_encFps;
        param->bOutOverwrite = false;
        auto sts = filter->init(param, m_pQSVLog);
        if (sts != RGY_ERR_NONE) {
            return sts;
        }
        //入力フレーム情報を更新
        inputFrame = param->frameOut;
        m_encFps = param->baseFps;
        //登録
        clfilters.push_back(std::move(filter));
        return RGY_ERR_NONE;
    }
    //字幕焼きこみ
    if (vppType == VppType::CL_SUBBURN) {
        std::vector<std::unique_ptr<RGYFilter>> filters;
        for (const auto& subburn : params->vpp.subburn) {
#if ENABLE_AVSW_READER
            if (subburn.filename.length() > 0
                && m_trimParam.list.size() > 0) {
                PrintMes(RGY_LOG_ERROR, _T("--vpp-subburn with input as file cannot be used with --trim.\n"));
                return RGY_ERR_UNSUPPORTED;
            }
            unique_ptr<RGYFilter> filter(new RGYFilterSubburn(m_cl));
            shared_ptr<RGYFilterParamSubburn> param(new RGYFilterParamSubburn());
            param->subburn = subburn;

            auto pAVCodecReader = std::dynamic_pointer_cast<RGYInputAvcodec>(m_pFileReader);
            if (pAVCodecReader != nullptr) {
                param->videoInputStream = pAVCodecReader->GetInputVideoStream();
                param->videoInputFirstKeyPts = pAVCodecReader->GetVideoFirstKeyPts();
                for (const auto &stream : pAVCodecReader->GetInputStreamInfo()) {
                    if (stream.trackId == trackFullID(AVMEDIA_TYPE_SUBTITLE, param->subburn.trackId)) {
                        param->streamIn = stream;
                        break;
                    }
                }
                param->attachmentStreams = pAVCodecReader->GetInputAttachmentStreams();
            }
            param->videoInfo = m_pFileReader->GetInputFrameInfo();
            if (param->subburn.trackId != 0 && param->streamIn.stream == nullptr) {
                PrintMes(RGY_LOG_WARN, _T("Could not find subtitle track #%d, vpp-subburn for track #%d will be disabled.\n"),
                    param->subburn.trackId, param->subburn.trackId);
            } else {
                param->bOutOverwrite = true;
                param->videoOutTimebase = av_make_q(m_outputTimebase);
                param->frameIn = inputFrame;
                param->frameOut = inputFrame;
                param->baseFps = m_encFps;
                if (crop) param->crop = *crop;
                auto sts = filter->init(param, m_pQSVLog);
                if (sts != RGY_ERR_NONE) {
                    return sts;
                }
                //入力フレーム情報を更新
                inputFrame = param->frameOut;
                m_encFps = param->baseFps;
                clfilters.push_back(std::move(filter));
            }
#endif //#if ENABLE_AVSW_READER
        }
        return RGY_ERR_NONE;
    }
    //リサイズ
    if (vppType == VppType::CL_RESIZE) {
        auto filter = std::make_unique<RGYFilterResize>(m_cl);
        shared_ptr<RGYFilterParamResize> param(new RGYFilterParamResize());
        param->interp = (params->vpp.resize_algo != RGY_VPP_RESIZE_AUTO) ? params->vpp.resize_algo : RGY_VPP_RESIZE_SPLINE36;
        param->frameIn = inputFrame;
        param->frameOut = inputFrame;
        param->frameOut.width = resize.first;
        param->frameOut.height = resize.second;
        param->baseFps = m_encFps;
        param->bOutOverwrite = false;
        auto sts = filter->init(param, m_pQSVLog);
        if (sts != RGY_ERR_NONE) {
            return sts;
        }
        //入力フレーム情報を更新
        inputFrame = param->frameOut;
        m_encFps = param->baseFps;
        //登録
        clfilters.push_back(std::move(filter));
        return RGY_ERR_NONE;
    }
    //unsharp
    if (vppType == VppType::CL_UNSHARP) {
        unique_ptr<RGYFilter> filter(new RGYFilterUnsharp(m_cl));
        shared_ptr<RGYFilterParamUnsharp> param(new RGYFilterParamUnsharp());
        param->unsharp = params->vpp.unsharp;
        param->frameIn = inputFrame;
        param->frameOut = inputFrame;
        param->baseFps = m_encFps;
        param->bOutOverwrite = false;
        auto sts = filter->init(param, m_pQSVLog);
        if (sts != RGY_ERR_NONE) {
            return sts;
        }
        //入力フレーム情報を更新
        inputFrame = param->frameOut;
        m_encFps = param->baseFps;
        //登録
        clfilters.push_back(std::move(filter));
        return RGY_ERR_NONE;
    }
    //edgelevel
    if (vppType == VppType::CL_EDGELEVEL) {
        unique_ptr<RGYFilter> filter(new RGYFilterEdgelevel(m_cl));
        shared_ptr<RGYFilterParamEdgelevel> param(new RGYFilterParamEdgelevel());
        param->edgelevel = params->vpp.edgelevel;
        param->frameIn = inputFrame;
        param->frameOut = inputFrame;
        param->baseFps = m_encFps;
        param->bOutOverwrite = false;
        auto sts = filter->init(param, m_pQSVLog);
        if (sts != RGY_ERR_NONE) {
            return sts;
        }
        //入力フレーム情報を更新
        inputFrame = param->frameOut;
        m_encFps = param->baseFps;
        //登録
        clfilters.push_back(std::move(filter));
        return RGY_ERR_NONE;
    }
    //warpsharp
    if (vppType == VppType::CL_WARPSHARP) {
        unique_ptr<RGYFilter> filter(new RGYFilterWarpsharp(m_cl));
        shared_ptr<RGYFilterParamWarpsharp> param(new RGYFilterParamWarpsharp());
        param->warpsharp = params->vpp.warpsharp;
        param->frameIn = inputFrame;
        param->frameOut = inputFrame;
        param->baseFps = m_encFps;
        param->bOutOverwrite = false;
        auto sts = filter->init(param, m_pQSVLog);
        if (sts != RGY_ERR_NONE) {
            return sts;
        }
        //入力フレーム情報を更新
        inputFrame = param->frameOut;
        m_encFps = param->baseFps;
        //登録
        clfilters.push_back(std::move(filter));
        return RGY_ERR_NONE;
    }

    //tweak
    if (vppType == VppType::CL_TWEAK) {
        unique_ptr<RGYFilter> filter(new RGYFilterTweak(m_cl));
        shared_ptr<RGYFilterParamTweak> param(new RGYFilterParamTweak());
        param->tweak = params->vpp.tweak;
        param->frameIn = inputFrame;
        param->frameOut = inputFrame;
        param->baseFps = m_encFps;
        param->bOutOverwrite = true;
        auto sts = filter->init(param, m_pQSVLog);
        if (sts != RGY_ERR_NONE) {
            return sts;
        }
        //入力フレーム情報を更新
        inputFrame = param->frameOut;
        m_encFps = param->baseFps;
        //登録
        clfilters.push_back(std::move(filter));
        return RGY_ERR_NONE;
    }
    //deband
    if (vppType == VppType::CL_DEBAND) {
        unique_ptr<RGYFilter> filter(new RGYFilterDeband(m_cl));
        shared_ptr<RGYFilterParamDeband> param(new RGYFilterParamDeband());
        param->deband = params->vpp.deband;
        param->frameIn = inputFrame;
        param->frameOut = inputFrame;
        param->baseFps = m_encFps;
        param->bOutOverwrite = false;
        auto sts = filter->init(param, m_pQSVLog);
        if (sts != RGY_ERR_NONE) {
            return sts;
        }
        //入力フレーム情報を更新
        inputFrame = param->frameOut;
        m_encFps = param->baseFps;
        //登録
        clfilters.push_back(std::move(filter));
        return RGY_ERR_NONE;
    }
    //padding
    if (vppType == VppType::CL_PAD) {
        unique_ptr<RGYFilter> filter(new RGYFilterPad(m_cl));
        shared_ptr<RGYFilterParamPad> param(new RGYFilterParamPad());
        param->pad = params->vpp.pad;
        param->frameIn = inputFrame;
        param->frameOut = inputFrame;
        param->frameOut.width += params->vpp.pad.left + params->vpp.pad.right;
        param->frameOut.height += params->vpp.pad.top + params->vpp.pad.bottom;
        param->baseFps = m_encFps;
        param->bOutOverwrite = false;
        auto sts = filter->init(param, m_pQSVLog);
        if (sts != RGY_ERR_NONE) {
            return sts;
        }
        //入力フレーム情報を更新
        inputFrame = param->frameOut;
        m_encFps = param->baseFps;
        //登録
        clfilters.push_back(std::move(filter));
        return RGY_ERR_NONE;
    }

    PrintMes(RGY_LOG_ERROR, _T("Unknown filter type.\n"));
    return RGY_ERR_UNSUPPORTED;
}

RGY_ERR CQSVPipeline::createOpenCLCopyFilterForPreVideoMetric() {
    auto [err, outFrameInfo] = GetOutputVideoInfo();
    if (err != RGY_ERR_NONE) {
        PrintMes(RGY_LOG_ERROR, _T("Failed to get output frame info!\n"));
        return err;
    }

    const auto formatOut = videooutputinfo(outFrameInfo->videoPrm.mfx, m_VideoSignalInfo, m_chromalocInfo);
    std::unique_ptr<RGYFilter> filterCrop(new RGYFilterCspCrop(m_cl));
    std::shared_ptr<RGYFilterParamCrop> param(new RGYFilterParamCrop());
    param->frameOut = RGYFrameInfo(formatOut.dstWidth, formatOut.dstHeight, formatOut.csp, formatOut.bitdepth, formatOut.picstruct, RGY_MEM_TYPE_GPU);
    param->frameIn = param->frameOut;
    param->frameIn.bitdepth = RGY_CSP_BIT_DEPTH[param->frameIn.csp];
    param->baseFps = m_encFps;
    param->bOutOverwrite = false;
    auto sts = filterCrop->init(param, m_pQSVLog);
    if (sts != RGY_ERR_NONE) {
        return sts;
    }
    //登録
    std::vector<std::unique_ptr<RGYFilter>> filters;
    filters.push_back(std::move(filterCrop));
    if (m_vpFilters.size() > 0) {
        PrintMes(RGY_LOG_ERROR, _T("Unknown error, not expected that m_vpFilters has size.\n"));
        return RGY_ERR_UNDEFINED_BEHAVIOR;
    }
    m_vpFilters.push_back(std::move(VppVilterBlock(filters)));
    return RGY_ERR_NONE;
}

RGY_ERR CQSVPipeline::InitFilters(sInputParams *inputParam) {
    const bool cropRequired = cropEnabled(inputParam->input.crop)
        && m_pFileReader->getInputCodec() != RGY_CODEC_UNKNOWN;

    RGYFrameInfo inputFrame(inputParam->input.srcWidth, inputParam->input.srcHeight,
        (m_pFileReader->getInputCodec() == RGY_CODEC_UNKNOWN) ? inputParam->input.csp : m_mfxDEC->GetFrameOut().csp,
        (m_pFileReader->getInputCodec() == RGY_CODEC_UNKNOWN) ? inputParam->input.bitdepth : m_mfxDEC->GetFrameOut().bitdepth,
        inputParam->input.picstruct,
        RGY_MEM_TYPE_GPU_IMAGE_NORMALIZED);
    const auto input_sar = rgy_rational<int>(inputParam->input.sar[0], inputParam->input.sar[1]);
    const int croppedWidth = inputFrame.width - inputParam->input.crop.e.left - inputParam->input.crop.e.right;
    const int croppedHeight = inputFrame.height - inputParam->input.crop.e.bottom - inputParam->input.crop.e.up;
    if (!cropRequired) {
        //入力時にcrop済み
        inputFrame.width = croppedWidth;
        inputFrame.height = croppedHeight;
    }

    //出力解像度が設定されていない場合は、入力解像度と同じにする
    if (inputParam->input.dstWidth == 0) {
        inputParam->input.dstWidth = croppedWidth;
    }
    if (inputParam->input.dstHeight == 0) {
        inputParam->input.dstHeight = croppedHeight;
    }
    const bool cspConvRequired = inputFrame.csp != getEncoderCsp(inputParam);

    //リサイザの出力すべきサイズ
    int resizeWidth = croppedWidth;
    int resizeHeight = croppedHeight;
    m_encWidth = resizeWidth;
    m_encHeight = resizeHeight;
    //指定のリサイズがあればそのサイズに設定する
    if (inputParam->input.dstWidth > 0 && inputParam->input.dstHeight > 0) {
        m_encWidth = inputParam->input.dstWidth;
        m_encHeight = inputParam->input.dstHeight;
        resizeWidth = m_encWidth;
        resizeHeight = m_encHeight;
    }
    if (inputParam->vpp.pad.enable) {
        m_encWidth += inputParam->vpp.pad.right + inputParam->vpp.pad.left;
        m_encHeight += inputParam->vpp.pad.bottom + inputParam->vpp.pad.top;
    }

    RGY_VPP_RESIZE_TYPE resizeRequired = RGY_VPP_RESIZE_TYPE_NONE;
    if (croppedWidth != resizeWidth || croppedHeight != resizeHeight) {
        resizeRequired = getVppResizeType(inputParam->vpp.resize_algo);
        if (resizeRequired == RGY_VPP_RESIZE_TYPE_UNKNOWN) {
            PrintMes(RGY_LOG_ERROR, _T("Unknown resize type.\n"));
            return RGY_ERR_INVALID_VIDEO_PARAM;
        }
    }
    //リサイズアルゴリズムのパラメータはvpp側に設定されているので、設定をvppmfxに転写する
    inputParam->vppmfx.resizeInterp = resize_algo_rgy_to_enc(inputParam->vpp.resize_algo);
    inputParam->vppmfx.resizeMode = resize_mode_rgy_to_enc(inputParam->vpp.resize_mode);

    //フレームレートのチェック
    if (inputParam->input.fpsN == 0 || inputParam->input.fpsD == 0) {
        PrintMes(RGY_LOG_ERROR, _T("unable to parse fps data.\n"));
        return RGY_ERR_INVALID_VIDEO_PARAM;
    }
    m_encFps = rgy_rational<int>(inputParam->input.fpsN, inputParam->input.fpsD);

    if (inputParam->input.picstruct & RGY_PICSTRUCT_INTERLACED) {
        if (CheckParamList(inputParam->vppmfx.deinterlace, list_deinterlace, "vpp-deinterlace") != RGY_ERR_NONE) {
            return RGY_ERR_INVALID_VIDEO_PARAM;
        }
        if (inputParam->common.AVSyncMode == RGY_AVSYNC_FORCE_CFR
            && (inputParam->vppmfx.deinterlace == MFX_DEINTERLACE_IT
                || inputParam->vppmfx.deinterlace == MFX_DEINTERLACE_IT_MANUAL
                || inputParam->vppmfx.deinterlace == MFX_DEINTERLACE_BOB
                || inputParam->vppmfx.deinterlace == MFX_DEINTERLACE_AUTO_DOUBLE)) {
            PrintMes(RGY_LOG_ERROR, _T("--avsync forcecfr cannnot be used with deinterlace %s.\n"), get_chr_from_value(list_deinterlace, inputParam->vppmfx.deinterlace));
            return RGY_ERR_INVALID_VIDEO_PARAM;
        }
    }

    //インタレ解除の個数をチェック
    int deinterlacer = 0;
    if (inputParam->vppmfx.deinterlace != MFX_DEINTERLACE_NONE) deinterlacer++;
    if (inputParam->vpp.afs.enable) deinterlacer++;
    if (inputParam->vpp.nnedi.enable) deinterlacer++;
    //if (inputParam->vpp.yadif.enable) deinterlacer++;
    if (deinterlacer >= 2) {
        PrintMes(RGY_LOG_ERROR, _T("Activating 2 or more deinterlacer is not supported.\n"));
        return RGY_ERR_UNSUPPORTED;
    }
    //picStructの設定
    m_encPicstruct = (deinterlacer > 0) ? RGY_PICSTRUCT_FRAME : inputParam->input.picstruct;

    //VUI情報
    auto VuiFiltered = inputParam->input.vui;

    m_encVUI = inputParam->common.out_vui;
    m_encVUI.apply_auto(inputParam->input.vui, m_encHeight);

    m_vpFilters.clear();

    std::vector<VppType> filterPipeline = InitFiltersCreateVppList(inputParam, cspConvRequired, cropRequired, resizeRequired);
    if (filterPipeline.size() == 0) {
        PrintMes(RGY_LOG_DEBUG, _T("No filters required.\n"));
        return RGY_ERR_NONE;
    }
    const auto clfilterCount = std::count_if(filterPipeline.begin(), filterPipeline.end(), [](VppType type) { return getVppFilterType(type) == VppFilterType::FILTER_OPENCL; });
    if (!m_cl && clfilterCount > 0) {
        if (!inputParam->ctrl.enableOpenCL) {
            PrintMes(RGY_LOG_ERROR, _T("OpenCL filter not enabled.\n"));
        } else {
            PrintMes(RGY_LOG_ERROR, _T("OpenCL filter not supported on this platform: %s.\n"), CPU_GEN_STR[getCPUGen(&m_mfxSession)]);
        }
        return RGY_ERR_UNSUPPORTED;
    }
    // blocksize
    const int blocksize = inputParam->CodecId == MFX_CODEC_HEVC ? 32 : 16;
    //読み込み時のcrop
    sInputCrop *inputCrop = (cropRequired) ? &inputParam->input.crop : nullptr;
    const auto resize = std::make_pair(resizeWidth, resizeHeight);

    std::vector<std::unique_ptr<RGYFilter>> vppOpenCLFilters;
    for (size_t i = 0; i < filterPipeline.size(); i++) {
        const VppFilterType ftype0 = (i >= 1)                      ? getVppFilterType(filterPipeline[i-1]) : VppFilterType::FILTER_NONE;
        const VppFilterType ftype1 =                                 getVppFilterType(filterPipeline[i+0]);
        const VppFilterType ftype2 = (i+1 < filterPipeline.size()) ? getVppFilterType(filterPipeline[i+1]) : VppFilterType::FILTER_NONE;
        if (ftype1 == VppFilterType::FILTER_MFX) {
            auto [err, vppmfx] = AddFilterMFX(inputFrame, m_encFps, filterPipeline[i], &inputParam->vppmfx,
                getEncoderCsp(inputParam), getEncoderBitdepth(inputParam), inputCrop, resize, blocksize);
            inputCrop = nullptr;
            if (err != RGY_ERR_NONE) {
                return err;
            }
            if (vppmfx) {
                m_vpFilters.push_back(std::move(VppVilterBlock(vppmfx)));
            }
        } else if (ftype1 == VppFilterType::FILTER_OPENCL) {
            if (ftype0 != VppFilterType::FILTER_OPENCL || filterPipeline[i] == VppType::CL_CROP) { // 前のfilterがOpenCLでない場合、変換が必要
                auto filterCrop = std::make_unique<RGYFilterCspCrop>(m_cl);
                shared_ptr<RGYFilterParamCrop> param(new RGYFilterParamCrop());
                param->frameIn = inputFrame;
                param->frameOut = inputFrame;
                param->frameOut.csp = getEncoderCsp(inputParam);
                switch (param->frameOut.csp) { // OpenCLフィルタの内部形式への変換
                case RGY_CSP_NV12: param->frameOut.csp = RGY_CSP_YV12; break;
                case RGY_CSP_P010: param->frameOut.csp = RGY_CSP_YV12_16; break;
                case RGY_CSP_AYUV: param->frameOut.csp = RGY_CSP_YUV444; break;
                case RGY_CSP_Y410: param->frameOut.csp = RGY_CSP_YUV444_16; break;
                case RGY_CSP_Y416: param->frameOut.csp = RGY_CSP_YUV444_16; break;
                default:
                    break;
                }
                param->frameOut.bitdepth = RGY_CSP_BIT_DEPTH[param->frameOut.csp];
                if (inputCrop) {
                    param->crop = *inputCrop;
                    inputCrop = nullptr;
                }
                param->baseFps = m_encFps;
                param->frameOut.mem_type = RGY_MEM_TYPE_GPU;
                param->bOutOverwrite = false;
                auto sts = filterCrop->init(param, m_pQSVLog);
                if (sts != RGY_ERR_NONE) {
                    return sts;
                }
                //入力フレーム情報を更新
                inputFrame = param->frameOut;
                m_encFps = param->baseFps;
                vppOpenCLFilters.push_back(std::move(filterCrop));
            }
            if (filterPipeline[i] != VppType::CL_CROP) {
                auto err = AddFilterOpenCL(vppOpenCLFilters, inputFrame, m_encFps, filterPipeline[i], inputParam, inputCrop, resize);
                if (err != RGY_ERR_NONE) {
                    return err;
                }
            }
            if (ftype2 != VppFilterType::FILTER_OPENCL) { // 次のfilterがOpenCLでない場合、変換が必要
                std::unique_ptr<RGYFilter> filterCrop(new RGYFilterCspCrop(m_cl));
                std::shared_ptr<RGYFilterParamCrop> param(new RGYFilterParamCrop());
                param->frameIn = inputFrame;
                param->frameOut = inputFrame;
                param->frameOut.csp = getEncoderCsp(inputParam);
                param->frameOut.bitdepth = getEncoderBitdepth(inputParam);
                param->frameOut.mem_type = RGY_MEM_TYPE_GPU_IMAGE_NORMALIZED;
                param->baseFps = m_encFps;
                param->bOutOverwrite = false;
                auto sts = filterCrop->init(param, m_pQSVLog);
                if (sts != RGY_ERR_NONE) {
                    return sts;
                }
                //入力フレーム情報を更新
                inputFrame = param->frameOut;
                m_encFps = param->baseFps;
                //登録
                vppOpenCLFilters.push_back(std::move(filterCrop));
                // ブロックに追加する
                m_vpFilters.push_back(std::move(VppVilterBlock(vppOpenCLFilters)));
                vppOpenCLFilters.clear();
            }
        } else {
            PrintMes(RGY_LOG_ERROR, _T("Unsupported vpp filter type.\n"));
            return RGY_ERR_UNSUPPORTED;
        }
    }

    m_encWidth  = inputFrame.width;
    m_encHeight = inputFrame.height;

    return RGY_ERR_NONE;
}

RGY_ERR CQSVPipeline::InitSessionInitParam(int threads, int priority) {
    INIT_MFX_EXT_BUFFER(m_ThreadsParam, MFX_EXTBUFF_THREADS_PARAM);
    m_ThreadsParam.NumThread = (mfxU16)clamp_param_int(threads, 0, QSV_SESSION_THREAD_MAX, _T("session-threads"));
    m_ThreadsParam.Priority = (mfxU16)clamp_param_int(priority, MFX_PRIORITY_LOW, MFX_PRIORITY_HIGH, _T("priority"));
    m_pInitParamExtBuf[0] = &m_ThreadsParam.Header;

    RGY_MEMSET_ZERO(m_InitParam);
    m_InitParam.ExtParam = m_pInitParamExtBuf;
    m_InitParam.NumExtParam = 1;
    return RGY_ERR_NONE;
}

#if defined(_WIN32) || defined(_WIN64)
typedef decltype(GetSystemInfo)* funcGetSystemInfo;
static int nGetSystemInfoHookThreads = -1;
static std::mutex mtxGetSystemInfoHook;
static funcGetSystemInfo origGetSystemInfoFunc = nullptr;
void __stdcall GetSystemInfoHook(LPSYSTEM_INFO lpSystemInfo) {
    origGetSystemInfoFunc(lpSystemInfo);
    if (lpSystemInfo && nGetSystemInfoHookThreads > 0) {
        decltype(lpSystemInfo->dwActiveProcessorMask) mask = 0;
        const int nThreads = std::max(1, std::min(nGetSystemInfoHookThreads, (int)sizeof(lpSystemInfo->dwActiveProcessorMask) * 8));
        for (int i = 0; i < nThreads; i++) {
            mask |= ((size_t)1<<i);
        }
        lpSystemInfo->dwActiveProcessorMask = mask;
        lpSystemInfo->dwNumberOfProcessors = nThreads;
    }
}
#endif

bool CQSVPipeline::preferD3D11Mode(const sInputParams *inputParam) {
#if defined(_WIN32) || defined(_WIN64)
    if (check_if_d3d11_necessary()) {
        return true;
    }

    const auto filters = InitFiltersCreateVppList(inputParam, inputParam->vpp.colorspace.convs.size() > 0, true, getVppResizeType(inputParam->vpp.resize_algo));
    const bool clfilterexists = std::find_if(filters.begin(), filters.end(), [](VppType filter) {
        return getVppFilterType(filter) == VppFilterType::FILTER_OPENCL;
    }) != filters.end();
    return clfilterexists;
#else
    return false;
#endif
}

RGY_ERR CQSVPipeline::InitSession(bool useHWLib, uint32_t memType) {
    auto err = RGY_ERR_NONE;
    m_SessionPlugins.reset();
    m_mfxSession.Close();
    PrintMes(RGY_LOG_DEBUG, _T("InitSession: Start initilaizing... memType: %s\n"), MemTypeToStr(memType));
#if defined(_WIN32) || defined(_WIN64)
    //コードの簡略化のため、静的フィールドを使うので、念のためロックをかける
    {
        std::lock_guard<std::mutex> lock(mtxGetSystemInfoHook);
        {
            nGetSystemInfoHookThreads = m_nMFXThreads;
            apihook api_hook;
            api_hook.hook(_T("kernel32.dll"), "GetSystemInfo", GetSystemInfoHook, (void **)&origGetSystemInfoFunc);
#endif

            auto InitSessionEx = [&](mfxIMPL impl, mfxVersion *verRequired) {
#if ENABLE_SESSION_THREAD_CONFIG
                if (m_ThreadsParam.NumThread != 0 || m_ThreadsParam.Priority != get_value_from_chr(list_priority, _T("normal"))) {
                    m_InitParam.Implementation = impl;
                    m_InitParam.Version = MFX_LIB_VERSION_1_15;
                    if (useHWLib) {
                        m_InitParam.GPUCopy = MFX_GPUCOPY_ON;
                    }
                    if (MFX_ERR_NONE == m_mfxSession.InitEx(m_InitParam)) {
                        return MFX_ERR_NONE;
                    } else {
                        m_ThreadsParam.NumThread = 0;
                        m_ThreadsParam.Priority = get_value_from_chr(list_priority, _T("normal"));
                    }
                }
#endif
                return err_to_rgy(m_mfxSession.Init(impl, verRequired));
            };

            if (useHWLib) {
                //とりあえず、MFX_IMPL_HARDWARE_ANYでの初期化を試みる
                mfxIMPL impl = MFX_IMPL_HARDWARE_ANY;
                m_memType = (memType) ? D3D9_MEMORY : SYSTEM_MEMORY;
#if MFX_D3D11_SUPPORT
                //Win7でD3D11のチェックをやると、
                //デスクトップコンポジションが切られてしまう問題が発生すると報告を頂いたので、
                //D3D11をWin8以降に限定
                if (!check_OS_Win8orLater()) {
                    memType &= (~D3D11_MEMORY);
                    PrintMes(RGY_LOG_DEBUG, _T("InitSession: OS is Win7, do not check for d3d11 mode.\n"));
                }

#endif //#if MFX_D3D11_SUPPORT
                //まずd3d11モードを試すよう設定されていれば、ますd3d11を試して、失敗したらd3d9での初期化を試みる
                for (int i_try_d3d11 = 0; i_try_d3d11 < 1 + (HW_MEMORY == (memType & HW_MEMORY)); i_try_d3d11++) {
#if D3D_SURFACES_SUPPORT
#if MFX_D3D11_SUPPORT
                    if (D3D11_MEMORY & memType) {
                        if (0 == i_try_d3d11) {
                            impl |= MFX_IMPL_VIA_D3D11; //d3d11モードも試す場合は、まずd3d11モードをチェック
                            impl &= (~MFX_IMPL_HARDWARE_ANY); //d3d11モードでは、MFX_IMPL_HARDWAREをまず試す
                            impl |= MFX_IMPL_HARDWARE;
                            m_memType = D3D11_MEMORY;
                            PrintMes(RGY_LOG_DEBUG, _T("InitSession: trying to init session for d3d11 mode.\n"));
                        } else {
                            impl &= ~MFX_IMPL_VIA_D3D11; //d3d11をオフにして再度テストする
                            impl |= MFX_IMPL_VIA_D3D9;
                            m_memType = D3D9_MEMORY;
                            PrintMes(RGY_LOG_DEBUG, _T("InitSession: trying to init session for d3d9 mode.\n"));
                        }
                    } else
#endif //#if MFX_D3D11_SUPPORT
                    if (D3D9_MEMORY & memType) {
                        impl |= MFX_IMPL_VIA_D3D9; //d3d11モードも試す場合は、まずd3d11モードをチェック
                    }
#endif //#if D3D_SURFACES_SUPPORT
                    mfxVersion verRequired = MFX_LIB_VERSION_1_1;

                    err = InitSessionEx(impl, &verRequired);
                    if (err != RGY_ERR_NONE) {
                        if (impl & MFX_IMPL_HARDWARE_ANY) {  //MFX_IMPL_HARDWARE_ANYがサポートされない場合もあり得るので、失敗したらこれをオフにしてもう一回試す
                            impl &= (~MFX_IMPL_HARDWARE_ANY);
                            impl |= MFX_IMPL_HARDWARE;
                        } else if (impl & MFX_IMPL_HARDWARE) {  //MFX_IMPL_HARDWAREで失敗したら、MFX_IMPL_HARDWARE_ANYでもう一回試す
                            impl &= (~MFX_IMPL_HARDWARE);
                            impl |= MFX_IMPL_HARDWARE_ANY;
                        }
                        PrintMes(RGY_LOG_DEBUG, _T("InitSession: failed to init session for multi GPU mode, retry by single GPU mode.\n"));
                        err = err_to_rgy(m_mfxSession.Init(impl, &verRequired));
                    }

                    //成功したらループを出る
                    if (err == RGY_ERR_NONE) {
                        break;
                    }
                }
                PrintMes(RGY_LOG_DEBUG, _T("InitSession: initialized using %s memory.\n"), MemTypeToStr(m_memType));
            } else {
                mfxIMPL impl = MFX_IMPL_SOFTWARE;
                mfxVersion verRequired = MFX_LIB_VERSION_1_1;
                err = InitSessionEx(impl, &verRequired);
                m_memType = SYSTEM_MEMORY;
                PrintMes(RGY_LOG_DEBUG, _T("InitSession: initialized with system memory.\n"));
            }
#if defined(_WIN32) || defined(_WIN64)
        }
    }
#endif
    if (err != RGY_ERR_NONE) {
        PrintMes(RGY_LOG_DEBUG, _T("InitSession: Failed to initialize session using %s memory: %s.\n"), MemTypeToStr(m_memType), get_err_mes(err));
        return err;
    }

    //使用できる最大のversionをチェック
    m_mfxSession.QueryVersion(&m_mfxVer);
    mfxIMPL impl;
    m_mfxSession.QueryIMPL(&impl);
    PrintMes(RGY_LOG_DEBUG, _T("InitSession: mfx lib version: %d.%d, impl 0x%x\n"), m_mfxVer.Major, m_mfxVer.Minor, impl);
    return err;
}

RGY_ERR CQSVPipeline::InitVideoQualityMetric(sInputParams *prm) {
    if (prm->common.metric.enabled()) {
        if (!m_pmfxENC) {
            PrintMes(RGY_LOG_WARN, _T("Encoder not enabled, %s calculation will be disabled.\n"), prm->common.metric.enabled_metric().c_str());
            return RGY_ERR_NONE;
        }
        auto [err, outFrameInfo] = GetOutputVideoInfo();
        if (err != RGY_ERR_NONE) {
            PrintMes(RGY_LOG_ERROR, _T("Failed to get output frame info!\n"));
            return err;
        }
        mfxIMPL impl;
        m_mfxSession.QueryIMPL(&impl);
        auto mfxdec = std::make_unique<QSVMfxDec>(m_hwdev.get(), m_pMFXAllocator.get(), m_mfxVer, impl, m_memType, m_pQSVLog);

        const auto formatOut = videooutputinfo(outFrameInfo->videoPrm.mfx, m_VideoSignalInfo, m_chromalocInfo);
        unique_ptr<RGYFilterSsim> filterSsim(new RGYFilterSsim(m_cl));
        shared_ptr<RGYFilterParamSsim> param(new RGYFilterParamSsim());
        param->input = formatOut;
        param->input.srcWidth = m_encWidth;
        param->input.srcHeight = m_encHeight;
        param->bitDepth = prm->outputDepth;
        param->frameIn = RGYFrameInfo(formatOut.dstWidth, formatOut.dstHeight, formatOut.csp, formatOut.bitdepth, formatOut.picstruct, RGY_MEM_TYPE_GPU_IMAGE_NORMALIZED);
        param->frameOut = param->frameIn;
        param->baseFps = m_encFps;
        param->bOutOverwrite = false;
        param->mfxDEC = std::move(mfxdec);
        param->allocator = m_pMFXAllocator.get();
        param->metric = prm->common.metric;
        auto sts = filterSsim->init(param, m_pQSVLog);
        if (sts != RGY_ERR_NONE) {
            return sts;
        }
        m_videoQualityMetric = std::move(filterSsim);
    }
    return RGY_ERR_NONE;
}

RGY_ERR CQSVPipeline::InitLog(sInputParams *pParams) {
    //ログの初期化
    m_pQSVLog.reset(new RGYLog(pParams->ctrl.logfile.c_str(), pParams->ctrl.loglevel));
    if ((pParams->ctrl.logfile.length() > 0 || pParams->common.outputFilename.length() > 0) && pParams->input.type != RGY_INPUT_FMT_SM) {
        m_pQSVLog->writeFileHeader(pParams->common.outputFilename.c_str());
    }
    return RGY_ERR_NONE;
}

RGY_ERR CQSVPipeline::InitPerfMonitor(const sInputParams *inputParam) {
    const bool bLogOutput = inputParam->ctrl.perfMonitorSelect || inputParam->ctrl.perfMonitorSelectMatplot;
    tstring perfMonLog;
    if (bLogOutput) {
        perfMonLog = inputParam->common.outputFilename + _T("_perf.csv");
    }
    CPerfMonitorPrm perfMonitorPrm;
    if (m_pPerfMonitor->init(perfMonLog.c_str(), inputParam->pythonPath.c_str(), (bLogOutput) ? inputParam->ctrl.perfMonitorInterval : 1000,
        (int)inputParam->ctrl.perfMonitorSelect, (int)inputParam->ctrl.perfMonitorSelectMatplot,
#if defined(_WIN32) || defined(_WIN64)
        std::unique_ptr<void, handle_deleter>(OpenThread(SYNCHRONIZE | THREAD_QUERY_INFORMATION, false, GetCurrentThreadId()), handle_deleter()),
#else
        nullptr,
#endif
        m_pQSVLog, &perfMonitorPrm)) {
        PrintMes(RGY_LOG_WARN, _T("Failed to initialize performance monitor, disabled.\n"));
        m_pPerfMonitor.reset();
    }
    return RGY_ERR_NONE;
}

RGY_ERR CQSVPipeline::SetPerfMonitorThreadHandles() {
#if ENABLE_AVSW_READER
    if (m_pPerfMonitor) {
        HANDLE thOutput = NULL;
        HANDLE thInput = NULL;
        HANDLE thAudProc = NULL;
        HANDLE thAudEnc = NULL;
        auto pAVCodecReader = std::dynamic_pointer_cast<RGYInputAvcodec>(m_pFileReader);
        if (pAVCodecReader != nullptr) {
            thInput = pAVCodecReader->getThreadHandleInput();
        }
        auto pAVCodecWriter = std::dynamic_pointer_cast<RGYOutputAvcodec>(m_pFileWriter);
        if (pAVCodecWriter != nullptr) {
            thOutput = pAVCodecWriter->getThreadHandleOutput();
            thAudProc = pAVCodecWriter->getThreadHandleAudProcess();
            thAudEnc = pAVCodecWriter->getThreadHandleAudEncode();
        }
        m_pPerfMonitor->SetThreadHandles((HANDLE)NULL, thInput, thOutput, thAudProc, thAudEnc);
    }
#endif //#if ENABLE_AVSW_READER
    return RGY_ERR_NONE;
}

RGY_ERR CQSVPipeline::Init(sInputParams *pParams) {
    if (pParams == nullptr) {
        return RGY_ERR_NULL_PTR;
    }

    InitLog(pParams);

    RGY_ERR sts = RGY_ERR_NONE;

    if (pParams->bBenchmark) {
        pParams->common.AVMuxTarget = RGY_MUX_NONE;
        if (pParams->common.nAudioSelectCount) {
            for (int i = 0; i < pParams->common.nAudioSelectCount; i++) {
                rgy_free(pParams->common.ppAudioSelectList[i]);
            }
            rgy_free(pParams->common.ppAudioSelectList);
            pParams->common.nAudioSelectCount = 0;
            PrintMes(RGY_LOG_WARN, _T("audio copy or audio encoding disabled on benchmark mode.\n"));
        }
        if (pParams->common.nSubtitleSelectCount) {
            pParams->common.nSubtitleSelectCount = 0;
            PrintMes(RGY_LOG_WARN, _T("subtitle copy disabled on benchmark mode.\n"));
        }
        if (pParams->ctrl.perfMonitorSelect || pParams->ctrl.perfMonitorSelectMatplot) {
            pParams->ctrl.perfMonitorSelect = 0;
            pParams->ctrl.perfMonitorSelectMatplot = 0;
            PrintMes(RGY_LOG_WARN, _T("performance monitor disabled on benchmark mode.\n"));
        }
        pParams->common.muxOutputFormat = _T("raw");
        PrintMes(RGY_LOG_DEBUG, _T("Param adjusted for benchmark mode.\n"));
    }

    m_nMFXThreads = pParams->nSessionThreads;
    m_nAVSyncMode = pParams->common.AVSyncMode;

    sts = InitSessionInitParam(pParams->nSessionThreads, pParams->nSessionThreadPriority);
    if (sts < RGY_ERR_NONE) return sts;
    PrintMes(RGY_LOG_DEBUG, _T("InitSessionInitParam: Success.\n"));

    m_pPerfMonitor = std::make_unique<CPerfMonitor>();

    sts = InitInput(pParams);
    if (sts < RGY_ERR_NONE) return sts;
    PrintMes(RGY_LOG_DEBUG, _T("InitInput: Success.\n"));

    sts = CheckParam(pParams);
    if (sts != RGY_ERR_NONE) return sts;
    PrintMes(RGY_LOG_DEBUG, _T("CheckParam: Success.\n"));

    sts = InitSession(true, pParams->memType);
    RGY_ERR(sts, _T("Failed to initialize encode session."));
    PrintMes(RGY_LOG_DEBUG, _T("InitSession: Success.\n"));

    m_SessionPlugins = std::make_unique<CSessionPlugins>(m_mfxSession);

    sts = CreateAllocator();
    if (sts < RGY_ERR_NONE) return sts;

    sts = InitOpenCL(pParams->ctrl.enableOpenCL);
    if (sts < RGY_ERR_NONE) return sts;

    sts = InitMfxDecParams(pParams);
    if (sts < RGY_ERR_NONE) return sts;

    sts = InitFilters(pParams);
    if (sts < RGY_ERR_NONE) return sts;

    sts = InitMfxEncodeParams(pParams);
    if (sts < RGY_ERR_NONE) return sts;

    sts = InitChapters(pParams);
    if (sts < RGY_ERR_NONE) return sts;

    sts = InitPerfMonitor(pParams);
    if (sts < RGY_ERR_NONE) return sts;

    sts = InitOutput(pParams);
    if (sts < RGY_ERR_NONE) return sts;

    const int nPipelineElements = !!m_mfxDEC + (int)m_vpFilters.size() + !!m_pmfxENC;
    if (nPipelineElements == 0) {
        PrintMes(RGY_LOG_ERROR, _T("None of the pipeline element (DEC,VPP,ENC) are activated!\n"));
        return RGY_ERR_INVALID_VIDEO_PARAM;
    }
    PrintMes(RGY_LOG_DEBUG, _T("pipeline element count: %d\n"), nPipelineElements);

    m_nProcSpeedLimit = pParams->ctrl.procSpeedLimit;
    m_nAsyncDepth = clamp_param_int((pParams->ctrl.lowLatency) ? 1 : pParams->nAsyncDepth, 0, QSV_ASYNC_DEPTH_MAX, _T("async-depth"));
    if (m_nAsyncDepth == 0) {
        m_nAsyncDepth = QSV_DEFAULT_ASYNC_DEPTH;
        PrintMes(RGY_LOG_DEBUG, _T("async depth automatically set to %d\n"), m_nAsyncDepth);
    }
    if (pParams->ctrl.lowLatency) {
        pParams->bDisableTimerPeriodTuning = false;
    }

    sts = InitVideoQualityMetric(pParams);
    if (sts < RGY_ERR_NONE) return sts;

#if defined(_WIN32) || defined(_WIN64)
    if (!pParams->bDisableTimerPeriodTuning) {
        m_bTimerPeriodTuning = true;
        timeBeginPeriod(1);
        PrintMes(RGY_LOG_DEBUG, _T("timeBeginPeriod(1)\n"));
    }
#endif //#if defined(_WIN32) || defined(_WIN64)

    if ((sts = ResetMFXComponents(pParams)) != RGY_ERR_NONE) {
        return sts;
    }
    if ((sts = SetPerfMonitorThreadHandles()) != RGY_ERR_NONE) {
        PrintMes(RGY_LOG_ERROR, _T("Failed to set thread handles to perf monitor!\n"));
        return sts;
    }
    return RGY_ERR_NONE;
}

void CQSVPipeline::Close() {
    // MFXのコンポーネントをm_pipelineTasksの解放(フレームの解放)前に実施する
    PrintMes(RGY_LOG_DEBUG, _T("Clear vpp filters...\n"));
    m_videoQualityMetric.reset();
    m_vpFilters.clear();
    PrintMes(RGY_LOG_DEBUG, _T("Closing m_pmfxDEC/ENC/VPP...\n"));
    m_mfxDEC.reset();
    m_pmfxENC.reset();
    m_mfxVPP.clear();
    //この中でフレームの解放がなされる
    PrintMes(RGY_LOG_DEBUG, _T("Clear pipeline tasks and allocated frames...\n"));
    m_pipelineTasks.clear();

    PrintMes(RGY_LOG_DEBUG, _T("Closing enc status...\n"));
    m_pStatus.reset();

#if ENABLE_MVC_ENCODING
    FreeMVCSeqDesc();
#endif

    m_EncExtParams.clear();

    m_DecInputBitstream.clear();

    PrintMes(RGY_LOG_DEBUG, _T("Closing Plugins...\n"));
    m_SessionPlugins.reset();

    PrintMes(RGY_LOG_DEBUG, _T("Closing mfxSession...\n"));
    m_mfxSession.Close();

    PrintMes(RGY_LOG_DEBUG, _T("DeleteAllocator...\n"));
    // allocator if used as external for MediaSDK must be deleted after SDK components
    DeleteAllocator();

    m_trimParam.list.clear();
    m_trimParam.offset = 0;

    m_cl.reset();

    PrintMes(RGY_LOG_DEBUG, _T("Closing audio readers (if used)...\n"));
    m_AudioReaders.clear();

    for (auto pWriter : m_pFileWriterListAudio) {
        if (pWriter) {
            if (pWriter != m_pFileWriter) {
                pWriter->Close();
                pWriter.reset();
            }
        }
    }
    m_pFileWriterListAudio.clear();

    PrintMes(RGY_LOG_DEBUG, _T("Closing writer...\n"));
    if (m_pFileWriter) {
        m_pFileWriter->Close();
        m_pFileWriter.reset();
    }

    PrintMes(RGY_LOG_DEBUG, _T("Closing reader...\n"));
    if (m_pFileReader) {
        m_pFileReader->Close();
        m_pFileReader.reset();
    }
#if defined(_WIN32) || defined(_WIN64)
    if (m_bTimerPeriodTuning) {
        timeEndPeriod(1);
        m_bTimerPeriodTuning = false;
        PrintMes(RGY_LOG_DEBUG, _T("timeEndPeriod(1)\n"));
    }
#endif //#if defined(_WIN32) || defined(_WIN64)

    m_timecode.reset();

    PrintMes(RGY_LOG_DEBUG, _T("Closing perf monitor...\n"));
    m_pPerfMonitor.reset();

    m_nMFXThreads = -1;
    m_pAbortByUser = nullptr;
    m_nAVSyncMode = RGY_AVSYNC_ASSUME_CFR;
    m_nProcSpeedLimit = 0;
#if ENABLE_AVSW_READER
    av_qsv_log_free();
#endif //#if ENABLE_AVSW_READER
    PrintMes(RGY_LOG_DEBUG, _T("Closed pipeline.\n"));
    if (m_pQSVLog.get() != nullptr) {
        m_pQSVLog->writeFileFooter();
        m_pQSVLog.reset();
    }
}

int CQSVPipeline::logTemporarilyIgnoreErrorMes() {
    //MediaSDK内のエラーをRGY_LOG_DEBUG以下の時以外には一時的に無視するようにする。
    //RGY_LOG_DEBUG以下の時にも、「無視できるエラーが発生するかもしれない」ことをログに残す。
    const auto log_level = m_pQSVLog->getLogLevel();
    if (log_level >= RGY_LOG_MORE) {
        m_pQSVLog->setLogLevel(RGY_LOG_QUIET); //一時的にエラーを無視
    } else {
        PrintMes(RGY_LOG_DEBUG, _T("ResetMFXComponents: there might be error below, but it might be internal error which could be ignored.\n"));
    }
    return log_level;
}

RGY_ERR CQSVPipeline::InitMfxEncode() {
    if (!m_pmfxENC) {
        return RGY_ERR_NONE;
    }
    const auto log_level = logTemporarilyIgnoreErrorMes();
    m_prmSetIn.vidprm = m_mfxEncParams;
    m_prmSetIn.cop = m_CodingOption;
    m_prmSetIn.cop2 = m_CodingOption2;
    m_prmSetIn.cop3 = m_CodingOption3;
    m_prmSetIn.hevc = m_ExtHEVCParam;
    auto sts = err_to_rgy(m_pmfxENC->Init(&m_mfxEncParams));
    m_pQSVLog->setLogLevel(log_level);
    if (sts == RGY_WRN_PARTIAL_ACCELERATION) {
        PrintMes(RGY_LOG_WARN, _T("partial acceleration on Encoding.\n"));
        sts = RGY_ERR_NONE;
    }
    RGY_ERR(sts, _T("Failed to initialize encoder."));
    PrintMes(RGY_LOG_DEBUG, _T("Encoder initialized.\n"));
    return RGY_ERR_NONE;
}

RGY_ERR CQSVPipeline::InitMfxVpp() {
    for (auto& filterBlock : m_vpFilters) {
        if (filterBlock.type == VppFilterType::FILTER_MFX) {
            auto err = filterBlock.vppmfx->Init();
            if (err != RGY_ERR_NONE) {
                return err;
            }
        }
    }
    return RGY_ERR_NONE;
}

RGY_ERR CQSVPipeline::InitMfxDec() {
    if (!m_mfxDEC) {
        return RGY_ERR_NONE;
    }
    const auto log_level = logTemporarilyIgnoreErrorMes();
    auto sts = m_mfxDEC->Init();
    m_pQSVLog->setLogLevel(log_level);
    if (sts == RGY_WRN_PARTIAL_ACCELERATION) {
        PrintMes(RGY_LOG_WARN, _T("partial acceleration on decoding.\n"));
        sts = RGY_ERR_NONE;
    }
    RGY_ERR(sts, _T("Failed to initialize decoder.\n"));
    PrintMes(RGY_LOG_DEBUG, _T("Dec initialized.\n"));
    return RGY_ERR_NONE;
}

RGY_ERR CQSVPipeline::ResetMFXComponents(sInputParams* pParams) {
    if (!pParams) {
        return RGY_ERR_NULL_PTR;
    }

    auto err = RGY_ERR_NONE;
    PrintMes(RGY_LOG_DEBUG, _T("ResetMFXComponents: Start...\n"));

    m_pipelineTasks.clear();

    if (m_pmfxENC) {
        err = err_to_rgy(m_pmfxENC->Close());
        RGY_IGNORE_STS(err, RGY_ERR_NOT_INITIALIZED);
        RGY_ERR(err, _T("Failed to reset encoder (fail on closing)."));
        PrintMes(RGY_LOG_DEBUG, _T("ResetMFXComponents: Enc closed.\n"));
    }

    for (auto& filterBlock : m_vpFilters) {
        if (filterBlock.type == VppFilterType::FILTER_MFX) {
            err = filterBlock.vppmfx->Close();
            RGY_IGNORE_STS(err, RGY_ERR_NOT_INITIALIZED);
            RGY_ERR(err, _T("Failed to reset vpp (fail on closing)."));
            PrintMes(RGY_LOG_DEBUG, _T("ResetMFXComponents: Vpp closed.\n"));
        }
    }

    if (m_mfxDEC) {
        err = m_mfxDEC->Close();
        RGY_IGNORE_STS(err, RGY_ERR_NOT_INITIALIZED);
        RGY_ERR(err, _T("Failed to reset decoder (fail on closing)."));
        PrintMes(RGY_LOG_DEBUG, _T("ResetMFXComponents: Dec closed.\n"));
    }

    // free allocated frames
    //DeleteFrames();
    //PrintMes(RGY_LOG_DEBUG, _T("ResetMFXComponents: Frames deleted.\n"));

    if ((err = CreatePipeline()) != RGY_ERR_NONE) {
        return err;
    }
    if ((err = AllocFrames()) != RGY_ERR_NONE) {
        return err;
    }
    if ((err = InitMfxEncode()) != RGY_ERR_NONE) {
        return err;
    }
    if ((err = InitMfxVpp()) != RGY_ERR_NONE) {
        return err;
    }
    if ((err = InitMfxDec()) != RGY_ERR_NONE) {
        return err;
    }
    return RGY_ERR_NONE;
}

RGY_ERR CQSVPipeline::AllocateSufficientBuffer(mfxBitstream *pBS) {
    if (!pBS) {
        return RGY_ERR_NULL_PTR;
    }

    mfxVideoParam par = { 0 };
    auto err = err_to_rgy(m_pmfxENC->GetVideoParam(&par));
    RGY_ERR(err, _T("Failed to get required output buffer size from encoder."));

    err = err_to_rgy(mfxBitstreamExtend(pBS, par.mfx.BufferSizeInKB * 1000 * (std::max)(1, (int)par.mfx.BRCParamMultiplier)));
    if (err != RGY_ERR_NONE) {
        PrintMes(RGY_LOG_ERROR, _T("Failed to allocate memory for output bufffer: %s\n"), get_err_mes(err));
        mfxBitstreamClear(pBS);
        return err;
    }

    return RGY_ERR_NONE;
}

RGY_ERR CQSVPipeline::Run() {
    return RunEncode2();
}

RGY_ERR CQSVPipeline::CreatePipeline() {
    m_pipelineTasks.clear();

    if (m_pFileReader->getInputCodec() == RGY_CODEC_UNKNOWN) {
        m_pipelineTasks.push_back(std::make_unique<PipelineTaskInput>(&m_mfxSession, m_pMFXAllocator.get(), 0, m_pFileReader.get(), m_mfxVer, m_cl, m_pQSVLog));
    } else {
        auto err = err_to_rgy(m_mfxSession.JoinSession(m_mfxDEC->GetSession()));
        if (err != RGY_ERR_NONE) {
            PrintMes(RGY_LOG_ERROR, _T("Failed to join mfx vpp session: %s.\n"), get_err_mes(err));
            return err;
        }
        m_pipelineTasks.push_back(std::make_unique<PipelineTaskMFXDecode>(&m_mfxSession, 1, m_mfxDEC->mfxdec(), m_mfxDEC->mfxparams(), m_pFileReader.get(), m_mfxVer, m_pQSVLog));
    }
    if (m_pFileWriterListAudio.size() > 0) {
        m_pipelineTasks.push_back(std::make_unique<PipelineTaskAudio>(m_pFileReader.get(), m_AudioReaders, m_pFileWriterListAudio, m_vpFilters, 0, m_mfxVer, m_pQSVLog));
    }
    if (m_trimParam.list.size() > 0) {
        m_pipelineTasks.push_back(std::make_unique<PipelineTaskTrim>(m_trimParam, 0, m_mfxVer, m_pQSVLog));
    }

    const int64_t outFrameDuration = std::max<int64_t>(1, rational_rescale(1, m_inputFps.inv(), m_outputTimebase)); //固定fpsを仮定した時の1フレームのduration (スケール: m_outputTimebase)
    const auto inputFrameInfo = m_pFileReader->GetInputFrameInfo();
    const auto inputFpsTimebase = rgy_rational<int>((int)inputFrameInfo.fpsD, (int)inputFrameInfo.fpsN);
    const auto srcTimebase = (m_pFileReader->getInputTimebase().n() > 0 && m_pFileReader->getInputTimebase().is_valid()) ? m_pFileReader->getInputTimebase() : inputFpsTimebase;
    m_pipelineTasks.push_back(std::make_unique<PipelineTaskCheckPTS>(&m_mfxSession, srcTimebase, m_outputTimebase, outFrameDuration, m_nAVSyncMode, m_mfxVer, m_pQSVLog));

    for (auto& filterBlock : m_vpFilters) {
        if (filterBlock.type == VppFilterType::FILTER_MFX) {
            auto err = err_to_rgy(m_mfxSession.JoinSession(filterBlock.vppmfx->GetSession()));
            if (err != RGY_ERR_NONE) {
                PrintMes(RGY_LOG_ERROR, _T("Failed to join mfx vpp session: %s.\n"), get_err_mes(err));
                return err;
            }
            m_pipelineTasks.push_back(std::make_unique<PipelineTaskMFXVpp>(&m_mfxSession, 1, filterBlock.vppmfx->mfxvpp(), filterBlock.vppmfx->mfxparams(), filterBlock.vppmfx->mfxver(), m_pQSVLog));
        } else if (filterBlock.type == VppFilterType::FILTER_OPENCL) {
            if (!m_cl) {
                PrintMes(RGY_LOG_ERROR, _T("OpenCL not enabled, OpenCL filters cannot be used.\n"), CPU_GEN_STR[getCPUGen(&m_mfxSession)]);
                return RGY_ERR_UNSUPPORTED;
            }
            m_pipelineTasks.push_back(std::make_unique<PipelineTaskOpenCL>(filterBlock.vppcl, nullptr, m_cl, m_memType, m_pMFXAllocator.get(), &m_mfxSession, 1, m_pQSVLog));
        } else {
            PrintMes(RGY_LOG_ERROR, _T("Unknown filter type.\n"));
            return RGY_ERR_UNSUPPORTED;
        }
    }

    if (m_videoQualityMetric) {
        int prevtask = -1;
        for (int itask = (int)m_pipelineTasks.size() - 1; itask >= 0; itask--) {
            if (!m_pipelineTasks[itask]->isPassThrough()) {
                prevtask = itask;
                break;
            }
        }
        if (m_pipelineTasks[prevtask]->taskType() == PipelineTaskType::INPUT) {
            //inputと直接つながる場合はうまく処理できなくなる(うまく同期がとれない)
            //そこで、CopyのOpenCLフィルタを挟んでその中で処理する
            auto err = createOpenCLCopyFilterForPreVideoMetric();
            if (err != RGY_ERR_NONE) {
                PrintMes(RGY_LOG_ERROR, _T("Failed to join mfx vpp session: %s.\n"), get_err_mes(err));
                return err;
            } else if (m_vpFilters.size() != 1) {
                PrintMes(RGY_LOG_ERROR, _T("m_vpFilters.size() != 1.\n"));
                return RGY_ERR_UNDEFINED_BEHAVIOR;
            }
            m_pipelineTasks.push_back(std::make_unique<PipelineTaskOpenCL>(m_vpFilters.front().vppcl, m_videoQualityMetric.get(), m_cl, m_memType, m_pMFXAllocator.get(), &m_mfxSession, 1, m_pQSVLog));
        } else if (m_pipelineTasks[prevtask]->taskType() == PipelineTaskType::OPENCL) {
            auto taskOpenCL = dynamic_cast<PipelineTaskOpenCL*>(m_pipelineTasks[prevtask].get());
            if (taskOpenCL == nullptr) {
                PrintMes(RGY_LOG_ERROR, _T("taskOpenCL == nullptr.\n"));
                return RGY_ERR_UNDEFINED_BEHAVIOR;
            }
            taskOpenCL->setVideoQualityMetricFilter(m_videoQualityMetric.get());
        } else {
            m_pipelineTasks.push_back(std::make_unique<PipelineTaskVideoQualityMetric>(m_videoQualityMetric.get(), m_cl, m_memType, m_pMFXAllocator.get(), &m_mfxSession, 0, m_mfxVer, m_pQSVLog));
        }
    }
    if (m_pmfxENC) {
        m_pipelineTasks.push_back(std::make_unique<PipelineTaskMFXEncode>(&m_mfxSession, 1, m_pmfxENC.get(), m_mfxVer, m_mfxEncParams, m_timecode.get(), m_outputTimebase, m_pQSVLog));
    } else {
        m_pipelineTasks.push_back(std::make_unique<PipelineTaskOutputRaw>(&m_mfxSession, 1, m_mfxVer, m_pQSVLog));
    }

    if (m_pipelineTasks.size() == 0) {
        PrintMes(RGY_LOG_DEBUG, _T("Failed to create pipeline: size = 0.\n"));
        return RGY_ERR_INVALID_OPERATION;
    }

    PrintMes(RGY_LOG_DEBUG, _T("Created pipeline.\n"));
    for (auto& p : m_pipelineTasks) {
        PrintMes(RGY_LOG_DEBUG, _T("  %s\n"), p->print().c_str());
    }
    PrintMes(RGY_LOG_DEBUG, _T("\n"));
    return RGY_ERR_NONE;
}

RGY_ERR CQSVPipeline::RunEncode2() {
    PrintMes(RGY_LOG_DEBUG, _T("Encode Thread: RunEncode2...\n"));
    if (m_pipelineTasks.size() == 0) {
        PrintMes(RGY_LOG_DEBUG, _T("Failed to create pipeline: size = 0.\n"));
        return RGY_ERR_INVALID_OPERATION;
    }

#if defined(_WIN32) || defined(_WIN64)
    TCHAR handleEvent[256];
    _stprintf_s(handleEvent, QSVENCC_ABORT_EVENT, GetCurrentProcessId());
    auto heAbort = std::unique_ptr<std::remove_pointer<HANDLE>::type, handle_deleter>((HANDLE)CreateEvent(nullptr, TRUE, FALSE, handleEvent));
    auto checkAbort = [pabort = m_pAbortByUser, &heAbort]() { return ((pabort != nullptr && *pabort) || WaitForSingleObject(heAbort.get(), 0) == WAIT_OBJECT_0) ? true : false; };
#else
    auto checkAbort = [pabort = m_pAbortByUser]() { return  (pabort != nullptr && *pabort); };
#endif
    m_pStatus->SetStart();

    CProcSpeedControl speedCtrl(m_nProcSpeedLimit);

    auto requireSync = [this](const size_t itask) {
        if (itask + 1 >= m_pipelineTasks.size()) return true; // 次が最後のタスクの時

        size_t srctask = itask;
        if (m_pipelineTasks[srctask]->isPassThrough()) {
            for (size_t prevtask = srctask-1; prevtask >= 0; prevtask--) {
                if (!m_pipelineTasks[prevtask]->isPassThrough()) {
                    srctask = prevtask;
                    break;
                }
            }
        }
        for (size_t nexttask = itask+1; nexttask < m_pipelineTasks.size(); nexttask++) {
            if (!m_pipelineTasks[nexttask]->isPassThrough()) {
                return m_pipelineTasks[srctask]->requireSync(m_pipelineTasks[nexttask]->taskType());
            }
        }
        return true;
    };

    RGY_ERR err = RGY_ERR_NONE;
    auto setloglevel = [](RGY_ERR err) {
        if (err == RGY_ERR_NONE || err == RGY_ERR_MORE_DATA || err == RGY_ERR_MORE_SURFACE || err == RGY_ERR_MORE_BITSTREAM) return RGY_LOG_DEBUG;
        if (err > RGY_ERR_NONE) return RGY_LOG_WARN;
        return RGY_LOG_ERROR;
    };
    struct PipelineTaskData {
        size_t task;
        std::unique_ptr<PipelineTaskOutput> data;
        PipelineTaskData(size_t t) : task(t), data() {};
        PipelineTaskData(size_t t, std::unique_ptr<PipelineTaskOutput>& d) : task(t), data(std::move(d)) {};
    };
    std::deque<PipelineTaskData> dataqueue;
    {
        auto checkContinue = [&checkAbort](RGY_ERR& err) {
            if (checkAbort()) { err = RGY_ERR_ABORTED; return false; }
            return err >= RGY_ERR_NONE || err == RGY_ERR_MORE_DATA || err == RGY_ERR_MORE_SURFACE;
        };
        while (checkContinue(err)) {
            if (dataqueue.empty()) {
                speedCtrl.wait(m_pipelineTasks.front()->outputFrames());
                dataqueue.push_back(PipelineTaskData(0)); // デコード実行用
            }
            while (!dataqueue.empty()) {
                auto d = std::move(dataqueue.front());
                dataqueue.pop_front();
                if (d.task < m_pipelineTasks.size()) {
                    err = RGY_ERR_NONE;
                    auto& task = m_pipelineTasks[d.task];
                    err = task->sendFrame(d.data);
                    if (!checkContinue(err)) {
                        PrintMes(setloglevel(err), _T("Break in task %s: %s.\n"), task->print().c_str(), get_err_mes(err));
                        break;
                    }
                    if (err == RGY_ERR_NONE) {
                        auto output = task->getOutput(requireSync(d.task));
                        if (output.size() == 0) break;
                        //出てきたものは先頭に追加していく
                        std::for_each(output.rbegin(), output.rend(), [itask = d.task, &dataqueue](auto&& o) {
                            dataqueue.push_front(PipelineTaskData(itask + 1, o));
                        });
                    }
                } else { // pipelineの最終的なデータを出力
                    if ((err = d.data->write(m_pFileWriter.get(), m_pMFXAllocator.get(), (m_cl) ? &m_cl->queue() : nullptr, m_videoQualityMetric.get())) != RGY_ERR_NONE) {
                        PrintMes(RGY_LOG_ERROR, _T("failed to write output: %s.\n"), get_err_mes(err));
                        break;
                    }
                }
            }
            if (dataqueue.empty()) {
                // taskを前方からひとつづつ出力が残っていないかチェック(主にcheckptsの処理のため)
                for (size_t itask = 0; itask < m_pipelineTasks.size(); itask++) {
                    auto& task = m_pipelineTasks[itask];
                    auto output = task->getOutput(requireSync(itask));
                    if (output.size() > 0) {
                        //出てきたものは先頭に追加していく
                        std::for_each(output.rbegin(), output.rend(), [itask, &dataqueue](auto&& o) {
                            dataqueue.push_front(PipelineTaskData(itask + 1, o));
                            });
                        //checkptsの処理上、でてきたフレームはすぐに後続処理に渡したいのでbreak
                        break;
                    }
                }
            }
        }
    }
    // flush
    if (err == RGY_ERR_MORE_BITSTREAM) { // 読み込みの完了を示すフラグ
        err = RGY_ERR_NONE;
        for (auto& task : m_pipelineTasks) {
            task->setOutputMaxQueueSize(0); //flushのため
        }
        auto checkContinue = [&checkAbort](RGY_ERR& err) {
            if (checkAbort()) { err = RGY_ERR_ABORTED; return false; }
            return err >= RGY_ERR_NONE || err == RGY_ERR_MORE_SURFACE;
        };
        for (size_t flushedTaskSend = 0, flushedTaskGet = 0; flushedTaskGet < m_pipelineTasks.size(); ) { // taskを前方からひとつづつflushしていく
            err = RGY_ERR_NONE;
            if (flushedTaskSend == flushedTaskGet) {
                dataqueue.push_back(PipelineTaskData(flushedTaskSend)); //flush用
            }
            while (!dataqueue.empty() && checkContinue(err)) {
                auto d = std::move(dataqueue.front());
                dataqueue.pop_front();
                if (d.task < m_pipelineTasks.size()) {
                    err = RGY_ERR_NONE;
                    auto& task = m_pipelineTasks[d.task];
                    err = task->sendFrame(d.data);
                    if (!checkContinue(err)) {
                        if (d.task == flushedTaskSend) flushedTaskSend++;
                        break;
                    }
                    auto output = task->getOutput(requireSync(d.task));
                    if (output.size() == 0) break;
                    //出てきたものは先頭に追加していく
                    std::for_each(output.rbegin(), output.rend(), [itask = d.task, &dataqueue](auto&& o) {
                        dataqueue.push_front(PipelineTaskData(itask + 1, o));
                    });
                    RGY_IGNORE_STS(err, RGY_ERR_MORE_DATA); //VPPなどでsendFrameがRGY_ERR_MORE_DATAだったが、フレームが出てくる場合がある
                } else { // pipelineの最終的なデータを出力
                    if ((err = d.data->write(m_pFileWriter.get(), m_pMFXAllocator.get(), (m_cl) ? &m_cl->queue() : nullptr, m_videoQualityMetric.get())) != RGY_ERR_NONE) {
                        PrintMes(RGY_LOG_ERROR, _T("failed to write output: %s.\n"), get_err_mes(err));
                        break;
                    }
                }
            }
            if (dataqueue.empty()) {
                // taskを前方からひとつづつ出力が残っていないかチェック(主にcheckptsの処理のため)
                for (size_t itask = flushedTaskGet; itask < m_pipelineTasks.size(); itask++) {
                    auto& task = m_pipelineTasks[itask];
                    auto output = task->getOutput(requireSync(itask));
                    if (output.size() > 0) {
                        //出てきたものは先頭に追加していく
                        std::for_each(output.rbegin(), output.rend(), [itask, &dataqueue](auto&& o) {
                            dataqueue.push_front(PipelineTaskData(itask + 1, o));
                            });
                        //checkptsの処理上、でてきたフレームはすぐに後続処理に渡したいのでbreak
                        break;
                    } else if (itask == flushedTaskGet && flushedTaskGet < flushedTaskSend) {
                        flushedTaskGet++;
                    }
                }
            }
        }
    }

    if (m_videoQualityMetric) {
        PrintMes(RGY_LOG_DEBUG, _T("Flushing video quality metric calc.\n"));
        m_videoQualityMetric->addBitstream(nullptr);
    }

    // MFXのコンポーネントをm_pipelineTasksの解放(フレームの解放)前に実施する
    PrintMes(RGY_LOG_DEBUG, _T("Clear vpp filters...\n"));
    m_vpFilters.clear();
    PrintMes(RGY_LOG_DEBUG, _T("Closing m_pmfxDEC/ENC/VPP...\n"));
    m_mfxDEC.reset();
    m_pmfxENC.reset();
    m_mfxVPP.clear();
    //この中でフレームの解放がなされる
    PrintMes(RGY_LOG_DEBUG, _T("Clear pipeline tasks and allocated frames...\n"));
    m_pipelineTasks.clear();
    PrintMes(RGY_LOG_DEBUG, _T("Waiting for writer to finish...\n"));
    m_pFileWriter->WaitFin();
    PrintMes(RGY_LOG_DEBUG, _T("Write results...\n"));
    if (m_videoQualityMetric) {
        PrintMes(RGY_LOG_DEBUG, _T("Write video quality metric results...\n"));
        m_videoQualityMetric->showResult();
    }
    m_pStatus->WriteResults();
    PrintMes(RGY_LOG_DEBUG, _T("RunEncode2: finished.\n"));
    return (err == RGY_ERR_NONE || err == RGY_ERR_MORE_DATA || err == RGY_ERR_MORE_SURFACE || err == RGY_ERR_MORE_BITSTREAM || err > RGY_ERR_NONE) ? RGY_ERR_NONE : err;
}

void CQSVPipeline::PrintMes(int log_level, const TCHAR *format, ...) {
    if (m_pQSVLog.get() == nullptr) {
        if (log_level <= RGY_LOG_INFO) {
            return;
        }
    } else if (log_level < m_pQSVLog->getLogLevel()) {
        return;
    }

    va_list args;
    va_start(args, format);

    int len = _vsctprintf(format, args) + 1; // _vscprintf doesn't count terminating '\0'
    vector<TCHAR> buffer(len, 0);
    _vstprintf_s(buffer.data(), len, format, args);
    va_end(args);

    if (m_pQSVLog.get() != nullptr) {
        m_pQSVLog->write(log_level, buffer.data());
    } else {
        _ftprintf(stderr, _T("%s"), buffer.data());
    }
}

void CQSVPipeline::GetEncodeLibInfo(mfxVersion *ver, bool *hardware) {
    if (NULL != ver && NULL != hardware) {
        mfxIMPL impl;
        m_mfxSession.QueryIMPL(&impl);
        *hardware = !!Check_HWUsed(impl);
        *ver = m_mfxVer;
    }

}

MemType CQSVPipeline::GetMemType() {
    return m_memType;
}

RGY_ERR CQSVPipeline::GetEncodeStatusData(EncodeStatusData *data) {
    if (data == nullptr)
        return RGY_ERR_NULL_PTR;

    if (m_pStatus == nullptr)
        return RGY_ERR_NOT_INITIALIZED;

    *data = m_pStatus->GetEncodeData();
    return RGY_ERR_NONE;
}

const TCHAR *CQSVPipeline::GetInputMessage() {
    return m_pFileReader->GetInputMessage();
}

std::pair<RGY_ERR, std::unique_ptr<QSVVideoParam>> CQSVPipeline::GetOutputVideoInfo() {
    auto prmset = std::make_unique<QSVVideoParam>(m_mfxEncParams.mfx.CodecId, m_mfxVer);
    if (m_pmfxENC) {
        auto sts = err_to_rgy(m_pmfxENC->GetVideoParam(&prmset->videoPrm));
        if (sts == RGY_ERR_NOT_INITIALIZED) { // 未初期化の場合、設定しようとしたパラメータで代用する
            prmset->videoPrm = m_mfxEncParams;
            sts = RGY_ERR_NONE;
        }
        return { sts, std::move(prmset) };
    }
    if (m_vpFilters.size() > 0) {
        prmset->isVppParam = true;
        auto& lastFilter = m_vpFilters.back();
        if (lastFilter.type == VppFilterType::FILTER_MFX) {
            auto sts = err_to_rgy(lastFilter.vppmfx->mfxvpp()->GetVideoParam(&prmset->videoPrmVpp));
            if (sts == RGY_ERR_NOT_INITIALIZED) { // 未初期化の場合、設定しようとしたパラメータで代用する
                prmset->videoPrmVpp = lastFilter.vppmfx->mfxparams();
                sts = RGY_ERR_NONE;
            }
            return { sts, std::move(prmset) };
        } else if (lastFilter.type == VppFilterType::FILTER_OPENCL) {
            auto& frameOut = lastFilter.vppcl.back()->GetFilterParam()->frameOut;
            const int blockSize = (m_mfxEncParams.mfx.CodecId == MFX_CODEC_HEVC) ? 32 : 16;
            prmset->videoPrmVpp.vpp.Out = frameinfo_rgy_to_enc(frameOut, m_encFps, rgy_rational<int>(0, 0), blockSize);
        } else {
            PrintMes(RGY_LOG_ERROR, _T("GetOutputVideoInfo: Unknown VPP filter type.\n"));
            return { RGY_ERR_UNSUPPORTED, std::move(prmset) };
        }
    }
    if (m_mfxDEC) {
        prmset->videoPrm = m_mfxDEC->mfxparams();
        return { RGY_ERR_NONE, std::move(prmset) };
    }
    PrintMes(RGY_LOG_ERROR, _T("GetOutputVideoInfo: None of the pipeline elements are detected!\n"));
    return { RGY_ERR_UNSUPPORTED, std::move(prmset) };
}

RGY_ERR CQSVPipeline::CheckCurrentVideoParam(TCHAR *str, mfxU32 bufSize) {
    mfxIMPL impl;
    m_mfxSession.QueryIMPL(&impl);

    mfxFrameInfo DstPicInfo = m_mfxEncParams.mfx.FrameInfo;

    auto [ err, outFrameInfo ] = GetOutputVideoInfo();
    if (err != RGY_ERR_NONE) {
        PrintMes(RGY_LOG_ERROR, _T("Failed to get output frame info!\n"));
        return err;
    }

    DstPicInfo = (outFrameInfo->isVppParam) ? outFrameInfo->videoPrmVpp.vpp.Out : outFrameInfo->videoPrm.mfx.FrameInfo;

    const int workSurfaceCount = std::accumulate(m_pipelineTasks.begin(), m_pipelineTasks.end(), 0, [](int sum, std::unique_ptr<PipelineTask>& task) {
        return sum + (int)task->workSurfacesCount();
        });


    if (m_pmfxENC) {
        mfxParamSet prmSetOut;
        prmSetOut.vidprm = outFrameInfo->videoPrm;
        prmSetOut.cop    = outFrameInfo->cop;
        prmSetOut.cop2   = outFrameInfo->cop2;
        prmSetOut.cop3   = outFrameInfo->cop3;
        prmSetOut.hevc   = outFrameInfo->hevcPrm;

        CompareParam(m_prmSetIn, prmSetOut);
    }

    TCHAR cpuInfo[256] = { 0 };
    getCPUInfo(cpuInfo, _countof(cpuInfo), &m_mfxSession);

    TCHAR gpu_info[1024] = { 0 };
    if (Check_HWUsed(impl)) {
        getGPUInfo("Intel", gpu_info, _countof(gpu_info));
    }
    TCHAR info[4096] = { 0 };
    mfxU32 info_len = 0;

#define PRINT_INFO(fmt, ...) { info_len += _stprintf_s(info + info_len, _countof(info) - info_len, fmt, __VA_ARGS__); }
#define PRINT_INT_AUTO(fmt, i) { if ((i) != 0) { info_len += _stprintf_s(info + info_len, _countof(info) - info_len, fmt, i); } else { info_len += _stprintf_s(info + info_len, _countof(info) - info_len, (fmt[_tcslen(fmt)-1]=='\n') ? _T("Auto\n") : _T("Auto")); } }
    PRINT_INFO(    _T("%s\n"), get_encoder_version());
#if defined(_WIN32) || defined(_WIN64)
    OSVERSIONINFOEXW osversioninfo = { 0 };
    tstring osversionstr = getOSVersion(&osversioninfo);
    PRINT_INFO(    _T("OS             %s %s (%d) [%s]\n"), osversionstr.c_str(), rgy_is_64bit_os() ? _T("x64") : _T("x86"), osversioninfo.dwBuildNumber, getACPCodepageStr().c_str());
#else
    PRINT_INFO(    _T("OS             %s %s\n"), getOSVersion().c_str(), rgy_is_64bit_os() ? _T("x64") : _T("x86"));
#endif
    PRINT_INFO(    _T("CPU Info       %s\n"), cpuInfo);
    if (Check_HWUsed(impl)) {
        PRINT_INFO(_T("GPU Info       %s\n"), gpu_info);
    }
    if (Check_HWUsed(impl)) {
        static const TCHAR * const NUM_APPENDIX[] = { _T("st"), _T("nd"), _T("rd"), _T("th")};
        mfxU32 iGPUID = GetAdapterID(m_mfxSession);
        PRINT_INFO(    _T("Media SDK      QuickSyncVideo (hardware encoder)%s, %d%s GPU, API v%d.%d\n"),
            get_low_power_str(outFrameInfo->videoPrm.mfx.LowPower), iGPUID + 1, NUM_APPENDIX[clamp(iGPUID, 0, _countof(NUM_APPENDIX) - 1)], m_mfxVer.Major, m_mfxVer.Minor);
    } else {
        PRINT_INFO(    _T("Media SDK      software encoder, API v%d.%d\n"), m_mfxVer.Major, m_mfxVer.Minor);
    }
    PRINT_INFO(    _T("Async Depth    %d frames\n"), m_nAsyncDepth);
    PRINT_INFO(    _T("Buffer Memory  %s, %d work buffer\n"), MemTypeToStr(m_memType), workSurfaceCount);
    //PRINT_INFO(    _T("Input Frame Format   %s\n"), ColorFormatToStr(m_pFileReader->m_ColorFormat));
    //PRINT_INFO(    _T("Input Frame Type     %s\n"), list_interlaced_mfx[get_cx_index(list_interlaced_mfx, SrcPicInfo.PicStruct)].desc);
    tstring inputMes = m_pFileReader->GetInputMessage();
    for (const auto& reader : m_AudioReaders) {
        inputMes += _T("\n") + tstring(reader->GetInputMessage());
    }
    auto inputMesSplitted = split(inputMes, _T("\n"));
    for (mfxU32 i = 0; i < inputMesSplitted.size(); i++) {
        PRINT_INFO(_T("%s%s\n"), (i == 0) ? _T("Input Info     ") : _T("               "), inputMesSplitted[i].c_str());
    }

    if (m_vpFilters.size() > 0) {
        const TCHAR *m = _T("VPP            ");
        tstring vppstr;
        for (auto& block : m_vpFilters) {
            if (block.type == VppFilterType::FILTER_MFX) {
                vppstr += block.vppmfx->print();
            } else if (block.type == VppFilterType::FILTER_OPENCL) {
                for (auto& clfilter : block.vppcl) {
                    vppstr += str_replace(clfilter->GetInputMessage(), _T("\n               "), _T("\n")) + _T("\n");
                }
            } else {
                PrintMes(RGY_LOG_ERROR, _T("CheckCurrentVideoParam: Unknown VPP filter type.\n"));
                return RGY_ERR_UNSUPPORTED;
            }
        }
        std::vector<TCHAR> vpp_mes(vppstr.length() + 1, _T('\0'));
        memcpy(vpp_mes.data(), vppstr.c_str(), vpp_mes.size() * sizeof(vpp_mes[0]));
        for (TCHAR *p = vpp_mes.data(), *q; (p = _tcstok_s(p, _T("\n"), &q)) != NULL; ) {
            PRINT_INFO(_T("%s%s\n"), m, p);
            m    = _T("               ");
            p = NULL;
        }
        if (m_videoQualityMetric) {
            PRINT_INFO(_T("%s%s\n"), m, m_videoQualityMetric->GetInputMessage().c_str());
        }
    }
    if (m_trimParam.list.size()
        && !(m_trimParam.list[0].start == 0 && m_trimParam.list[0].fin == TRIM_MAX)) {
        PRINT_INFO(_T("%s"), _T("Trim           "));
        for (auto trim : m_trimParam.list) {
            if (trim.fin == TRIM_MAX) {
                PRINT_INFO(_T("%d-fin "), trim.start + m_trimParam.offset);
            } else {
                PRINT_INFO(_T("%d-%d "), trim.start + m_trimParam.offset, trim.fin + m_trimParam.offset);
            }
        }
        PRINT_INFO(_T("[offset: %d]\n"), m_trimParam.offset);
    }
    PRINT_INFO(_T("AVSync         %s\n"), get_chr_from_value(list_avsync, m_nAVSyncMode));
    if (m_pmfxENC) {
        PRINT_INFO(_T("Output         %s%s %s @ Level %s%s\n"), CodecIdToStr(outFrameInfo->videoPrm.mfx.CodecId),
            (outFrameInfo->videoPrm.mfx.FrameInfo.BitDepthLuma > 8) ? strsprintf(_T("(%s %dbit)"), ChromaFormatToStr(outFrameInfo->videoPrm.mfx.FrameInfo.ChromaFormat), outFrameInfo->videoPrm.mfx.FrameInfo.BitDepthLuma).c_str()
                                                                    : strsprintf(_T("(%s)"), ChromaFormatToStr(outFrameInfo->videoPrm.mfx.FrameInfo.ChromaFormat)).c_str(),
            get_profile_list(outFrameInfo->videoPrm.mfx.CodecId)[get_cx_index(get_profile_list(outFrameInfo->videoPrm.mfx.CodecId), outFrameInfo->videoPrm.mfx.CodecProfile)].desc,
            get_level_list(outFrameInfo->videoPrm.mfx.CodecId)[get_cx_index(get_level_list(outFrameInfo->videoPrm.mfx.CodecId), outFrameInfo->videoPrm.mfx.CodecLevel & 0xff)].desc,
            (outFrameInfo->videoPrm.mfx.CodecId == MFX_CODEC_HEVC && (outFrameInfo->videoPrm.mfx.CodecLevel & MFX_TIER_HEVC_HIGH)) ? _T(" (high tier)") : _T(""));
    }
    PRINT_INFO(_T("%s         %dx%d%s %d:%d %0.3ffps (%d/%dfps)%s%s\n"),
        (m_pmfxENC) ? _T("      ") : _T("Output"),
        DstPicInfo.CropW, DstPicInfo.CropH, (DstPicInfo.PicStruct & MFX_PICSTRUCT_PROGRESSIVE) ? _T("p") : _T("i"),
        outFrameInfo->videoPrm.mfx.FrameInfo.AspectRatioW, outFrameInfo->videoPrm.mfx.FrameInfo.AspectRatioH,
        DstPicInfo.FrameRateExtN / (double)DstPicInfo.FrameRateExtD, DstPicInfo.FrameRateExtN, DstPicInfo.FrameRateExtD,
        (DstPicInfo.PicStruct & MFX_PICSTRUCT_PROGRESSIVE) ? _T("") : _T(", "),
        (DstPicInfo.PicStruct & MFX_PICSTRUCT_PROGRESSIVE) ? _T("") : list_interlaced_mfx[get_cx_index(list_interlaced_mfx, DstPicInfo.PicStruct)].desc);
    if (m_pFileWriter) {
        inputMesSplitted = split(m_pFileWriter->GetOutputMessage(), _T("\n"));
        for (auto mes : inputMesSplitted) {
            if (mes.length()) {
                PRINT_INFO(_T("%s%s\n"), _T("               "), mes.c_str());
            }
        }
    }
    for (auto pWriter : m_pFileWriterListAudio) {
        if (pWriter && pWriter != m_pFileWriter) {
            inputMesSplitted = split(pWriter->GetOutputMessage(), _T("\n"));
            for (auto mes : inputMesSplitted) {
                if (mes.length()) {
                    PRINT_INFO(_T("%s%s\n"), _T("               "), mes.c_str());
                }
            }
        }
    }

    if (m_pmfxENC) {
        PRINT_INFO(_T("Target usage   %s\n"), TargetUsageToStr(outFrameInfo->videoPrm.mfx.TargetUsage));
        PRINT_INFO(_T("Encode Mode    %s\n"), EncmodeToStr(outFrameInfo->videoPrm.mfx.RateControlMethod));
        if (m_mfxEncParams.mfx.RateControlMethod == MFX_RATECONTROL_CQP) {
            PRINT_INFO(_T("CQP Value      I:%d  P:%d  B:%d\n"), outFrameInfo->videoPrm.mfx.QPI, outFrameInfo->videoPrm.mfx.QPP, outFrameInfo->videoPrm.mfx.QPB);
        } else if (rc_is_type_lookahead(m_mfxEncParams.mfx.RateControlMethod)) {
            if (m_mfxEncParams.mfx.RateControlMethod != MFX_RATECONTROL_LA_ICQ) {
                PRINT_INFO(_T("Bitrate        %d kbps\n"), outFrameInfo->videoPrm.mfx.TargetKbps * (std::max<int>)(m_mfxEncParams.mfx.BRCParamMultiplier, 1));
                PRINT_INFO(_T("%s"), _T("Max Bitrate    "));
                PRINT_INT_AUTO(_T("%d kbps\n"), outFrameInfo->videoPrm.mfx.MaxKbps * (std::max<int>)(m_mfxEncParams.mfx.BRCParamMultiplier, 1));
            }
            PRINT_INFO(_T("Lookahead      depth %d frames"), outFrameInfo->cop2.LookAheadDepth);
            if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_8)) {
                PRINT_INFO(_T(", quality %s"), list_lookahead_ds[get_cx_index(list_lookahead_ds, outFrameInfo->cop2.LookAheadDS)].desc);
            }
            PRINT_INFO(_T("%s"), _T("\n"));
            if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_11)) {
                if (outFrameInfo->cop3.WinBRCSize) {
                    PRINT_INFO(_T("Windowed RC    %d frames, Max %d kbps\n"), outFrameInfo->cop3.WinBRCSize, outFrameInfo->cop3.WinBRCMaxAvgKbps);
                } else {
                    PRINT_INFO(_T("%s"), _T("Windowed RC    off\n"));
                }
            }
            if (m_mfxEncParams.mfx.RateControlMethod == MFX_RATECONTROL_LA_ICQ) {
                PRINT_INFO(_T("ICQ Quality    %d\n"), outFrameInfo->videoPrm.mfx.ICQQuality);
            }
        } else if (m_mfxEncParams.mfx.RateControlMethod == MFX_RATECONTROL_ICQ) {
            PRINT_INFO(_T("ICQ Quality    %d\n"), outFrameInfo->videoPrm.mfx.ICQQuality);
        } else {
            PRINT_INFO(_T("Bitrate        %d kbps\n"), outFrameInfo->videoPrm.mfx.TargetKbps * (std::max<int>)(m_mfxEncParams.mfx.BRCParamMultiplier, 1));
            if (m_mfxEncParams.mfx.RateControlMethod == MFX_RATECONTROL_AVBR) {
                //PRINT_INFO(_T("AVBR Accuracy range\t%.01lf%%"), m_mfxEncParams.mfx.Accuracy / 10.0);
                PRINT_INFO(_T("AVBR Converge  %d frames unit\n"), outFrameInfo->videoPrm.mfx.Convergence * 100);
            } else {
                PRINT_INFO(_T("%s"), _T("Max Bitrate    "));
                PRINT_INT_AUTO(_T("%d kbps\n"), outFrameInfo->videoPrm.mfx.MaxKbps * (std::max<int>)(m_mfxEncParams.mfx.BRCParamMultiplier, 1));
                if (m_mfxEncParams.mfx.RateControlMethod == MFX_RATECONTROL_QVBR) {
                    PRINT_INFO(_T("QVBR Quality   %d\n"), outFrameInfo->cop3.QVBRQuality);
                }
            }
            if (outFrameInfo->videoPrm.mfx.BufferSizeInKB > 0) {
                PRINT_INFO(_T("VBV Bufsize    %d kbps\n"), outFrameInfo->videoPrm.mfx.BufferSizeInKB * 8 * (std::max<int>)(m_mfxEncParams.mfx.BRCParamMultiplier, 1));
            }
        }
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_9)) {
            auto qp_limit_str = [](mfxU8 limitI, mfxU8 limitP, mfxU8 limitB) {
                mfxU8 limit[3] = { limitI, limitP, limitB };
                if (0 == (limit[0] | limit[1] | limit[2])) {
                    return tstring(_T("none"));
                }
                if (limit[0] == limit[1] && limit[0] == limit[2]) {
                    return strsprintf(_T("%d"), limit[0]);
                }

                tstring buf;
                for (int i = 0; i < 3; i++) {
                    buf += ((i) ? _T(":") : _T(""));
                    if (limit[i]) {
                        buf += strsprintf(_T("%d"), limit[i]);
                    } else {
                        buf += _T("-");
                    }
                }
                return buf;
            };
            PRINT_INFO(_T("QP Limit       min: %s, max: %s\n"),
                qp_limit_str(outFrameInfo->cop2.MinQPI, outFrameInfo->cop2.MinQPP, outFrameInfo->cop2.MinQPB).c_str(),
                qp_limit_str(outFrameInfo->cop2.MaxQPI, outFrameInfo->cop2.MaxQPP, outFrameInfo->cop2.MaxQPB).c_str());
        }
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_7)) {
            PRINT_INFO(_T("Trellis        %s\n"), list_avc_trellis[get_cx_index(list_avc_trellis_for_options, outFrameInfo->cop2.Trellis)].desc);
        }

        if (outFrameInfo->videoPrm.mfx.CodecId == MFX_CODEC_AVC && !Check_HWUsed(impl)) {
            PRINT_INFO(_T("CABAC          %s\n"), (outFrameInfo->cop.CAVLC == MFX_CODINGOPTION_ON) ? _T("off") : _T("on"));
            PRINT_INFO(_T("RDO            %s\n"), (outFrameInfo->cop.RateDistortionOpt == MFX_CODINGOPTION_ON) ? _T("on") : _T("off"));
            if ((outFrameInfo->cop.MVSearchWindow.x | outFrameInfo->cop.MVSearchWindow.y) == 0) {
                PRINT_INFO(_T("mv search      precision: %s\n"), list_mv_presicion[get_cx_index(list_mv_presicion, outFrameInfo->cop.MVPrecision)].desc);
            } else {
                PRINT_INFO(_T("mv search      precision: %s, window size:%dx%d\n"), list_mv_presicion[get_cx_index(list_mv_presicion, outFrameInfo->cop.MVPrecision)].desc, outFrameInfo->cop.MVSearchWindow.x, outFrameInfo->cop.MVSearchWindow.y);
            }
            PRINT_INFO(_T("min pred size  inter: %s   intra: %s\n"), list_pred_block_size[get_cx_index(list_pred_block_size, outFrameInfo->cop.InterPredBlockSize)].desc, list_pred_block_size[get_cx_index(list_pred_block_size, outFrameInfo->cop.IntraPredBlockSize)].desc);
        }
        PRINT_INFO(_T("%s"), _T("Ref frames     "));
        PRINT_INT_AUTO(_T("%d frames\n"), outFrameInfo->videoPrm.mfx.NumRefFrame);

        PRINT_INFO(_T("%s"), _T("Bframes        "));
        switch (outFrameInfo->videoPrm.mfx.GopRefDist) {
        case 0:  PRINT_INFO(_T("%s"), _T("Auto\n")); break;
        case 1:  PRINT_INFO(_T("%s"), _T("none\n")); break;
        default: PRINT_INFO(_T("%d frame%s%s%s\n"),
            outFrameInfo->videoPrm.mfx.GopRefDist - 1, (outFrameInfo->videoPrm.mfx.GopRefDist > 2) ? _T("s") : _T(""),
            check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_8) ? _T(", B-pyramid: ") : _T(""),
            (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_8) ? ((MFX_B_REF_PYRAMID == outFrameInfo->cop2.BRefType) ? _T("on") : _T("off")) : _T(""))); break;
        }

        //PRINT_INFO(    _T("Idr Interval    %d\n"), outFrameInfo->videoPrm.mfx.IdrInterval);
        PRINT_INFO(_T("%s"), _T("Max GOP Length "));
        PRINT_INT_AUTO(_T("%d frames\n"), outFrameInfo->videoPrm.mfx.GopPicSize);
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_8)) {
            //PRINT_INFO(    _T("GOP Structure           "));
            //bool adaptiveIOn = (MFX_CODINGOPTION_ON == outFrameInfo->cop2.AdaptiveI);
            //bool adaptiveBOn = (MFX_CODINGOPTION_ON == outFrameInfo->cop2.AdaptiveB);
            //if (!adaptiveIOn && !adaptiveBOn) {
            //    PRINT_INFO(_T("fixed\n"))
            //} else {
            //    PRINT_INFO(_T("Adaptive %s%s%s insert\n"),
            //        (adaptiveIOn) ? _T("I") : _T(""),
            //        (adaptiveIOn && adaptiveBOn) ? _T(",") : _T(""),
            //        (adaptiveBOn) ? _T("B") : _T(""));
            //}
        }
        if (outFrameInfo->videoPrm.mfx.NumSlice >= 2) {
            PRINT_INFO(_T("Slices         %d\n"), outFrameInfo->videoPrm.mfx.NumSlice);
        }

        if (outFrameInfo->videoPrm.mfx.CodecId == MFX_CODEC_VP8) {
            PRINT_INFO(_T("Sharpness      %d\n"), outFrameInfo->copVp8.SharpnessLevel);
        }
        { const auto &vui_str = m_encVUI.print_all();
        if (vui_str.length() > 0) {
            PRINT_INFO(_T("VUI            %s\n"), vui_str.c_str());
        }
        }
        if (m_HDRSei) {
            const auto masterdisplay = m_HDRSei->print_masterdisplay();
            const auto maxcll = m_HDRSei->print_maxcll();
            if (masterdisplay.length() > 0) {
                const tstring tstr = char_to_tstring(masterdisplay);
                const auto splitpos = tstr.find(_T("WP("));
                if (splitpos == std::string::npos) {
                    PRINT_INFO(_T("MasteringDisp  %s\n"), tstr.c_str());
                } else {
                    PRINT_INFO(_T("MasteringDisp  %s\n")
                               _T("               %s\n"),
                        tstr.substr(0, splitpos-1).c_str(), tstr.substr(splitpos).c_str());
                }
            }
            if (maxcll.length() > 0) {
                PRINT_INFO(_T("MaxCLL/MaxFALL %s\n"), char_to_tstring(maxcll).c_str());
            }
        }

        //last line
        tstring extFeatures;
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_6)) {
            if (outFrameInfo->cop2.MBBRC  == MFX_CODINGOPTION_ON) {
                extFeatures += _T("PerMBRC ");
            }
            if (outFrameInfo->cop2.ExtBRC == MFX_CODINGOPTION_ON) {
                extFeatures += _T("ExtBRC ");
            }
        }
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_9)) {
            if (outFrameInfo->cop2.DisableDeblockingIdc) {
                extFeatures += _T("No-Deblock ");
            }
            if (outFrameInfo->cop2.IntRefType) {
                extFeatures += _T("Intra-Refresh ");
            }
        }
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_13)) {
            if (outFrameInfo->cop3.DirectBiasAdjustment == MFX_CODINGOPTION_ON) {
                extFeatures += _T("DirectBiasAdjust ");
            }
            if (outFrameInfo->cop3.GlobalMotionBiasAdjustment == MFX_CODINGOPTION_ON) {
                extFeatures += strsprintf(_T("MVCostScaling=%d "), outFrameInfo->cop3.MVCostScalingFactor);
            }
        }
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_16)) {
            if (outFrameInfo->cop3.WeightedPred != MFX_WEIGHTED_PRED_UNKNOWN) {
                extFeatures += _T("WeightP ");
            }
            if (outFrameInfo->cop3.WeightedBiPred != MFX_WEIGHTED_PRED_UNKNOWN) {
                extFeatures += _T("WeightB ");
            }
        }
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_17)) {
            if (outFrameInfo->cop3.FadeDetection == MFX_CODINGOPTION_ON) {
                extFeatures += _T("FadeDetect ");
            }
        }
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_19)) {
            if (outFrameInfo->cop3.EnableQPOffset == MFX_CODINGOPTION_ON) {
                extFeatures += _T("QPOffset ");
            }
        }
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_26)) {
            if (outFrameInfo->cop3.ExtBrcAdaptiveLTR == MFX_CODINGOPTION_ON) {
                extFeatures += _T("AdaptiveLTR ");
            }
        }
        //if (outFrameInfo->cop.AUDelimiter == MFX_CODINGOPTION_ON) {
        //    extFeatures += _T("aud ");
        //}
        //if (outFrameInfo->cop.PicTimingSEI == MFX_CODINGOPTION_ON) {
        //    extFeatures += _T("pic_struct ");
        //}
        //if (outFrameInfo->cop.SingleSeiNalUnit == MFX_CODINGOPTION_ON) {
        //    extFeatures += _T("SingleSEI ");
        //}
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_23)) {
            if (outFrameInfo->cop3.RepartitionCheckEnable == MFX_CODINGOPTION_ON) {
                extFeatures += _T("RepartitionCheck ");
            }
        }
        if (m_mfxEncParams.mfx.CodecId == MFX_CODEC_HEVC) {
            if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_26)) {
                if (outFrameInfo->cop3.TransformSkip == MFX_CODINGOPTION_ON) {
                    extFeatures += _T("tskip ");
                }
                if (outFrameInfo->hevcPrm.LCUSize != 0) {
                    extFeatures += strsprintf(_T("ctu:%d "), outFrameInfo->hevcPrm.LCUSize);
                }
                if (outFrameInfo->hevcPrm.SampleAdaptiveOffset != 0) {
                    extFeatures += strsprintf(_T("sao:%s "), get_chr_from_value(list_hevc_sao, outFrameInfo->hevcPrm.SampleAdaptiveOffset));
                }
            }
        }
        if (extFeatures.length() > 0) {
            PRINT_INFO(_T("Ext. Features  %s\n"), extFeatures.c_str());
        }
    }

    PrintMes(RGY_LOG_INFO, info);
    if (str && bufSize > 0) {
        _tcscpy_s(str, bufSize, info);
    }

    return RGY_ERR_NONE;
#undef PRINT_INFO
#undef PRINT_INT_AUTO
}

