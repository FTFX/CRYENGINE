// Copyright 2001-2018 Crytek GmbH / Crytek Group. All rights reserved.

#include "StdAfx.h"
#include <CryString/UnicodeFunctions.h>
#include <CryCore/Platform/WindowsUtils.h>
#include <CrySystem/IProjectManager.h>
#include <CrySystem/ConsoleRegistration.h>
#include <CryFont/IFont.h>

#include "D3DStereo.h"
#include "D3DPostProcess.h"
#include "D3D_SVO.h"
#include "D3DREBreakableGlassBuffer.h"
#include "NullD3D11Device.h"
#include "PipelineProfiler.h"
#include <CryInput/IHardwareMouse.h>
#include <Common/RenderDisplayContext.h>
#include <Gpu/Particles/GpuParticleManager.h>
#include <GraphicsPipeline/ComputeSkinning.h>
#include <GraphicsPipeline/Common/SceneRenderPass.h>

#if CRY_PLATFORM_WINDOWS
	#if defined(USE_AMD_API)
		#include AMD_API_HEADER
LINK_THIRD_PARTY_LIBRARY(AMD_API_LIB)

		AGSContext* g_pAGSContext = nullptr;
		AGSConfiguration g_pAGSContextConfiguration = {};
		AGSGPUInfo g_gpuInfo;
		unsigned int g_extensionsSupported = 0;
	#endif

	#if defined(USE_NV_API)
		#include NV_API_HEADER
LINK_THIRD_PARTY_LIBRARY(NV_API_LIB)
	#endif

	#if defined(USE_AMD_EXT)
		#include <AMD/AMD_Extensions/AmdDxExtDepthBoundsApi.h>

		bool g_bDepthBoundsTest = true;
		IAmdDxExt* g_pExtension = NULL;
		IAmdDxExtDepthBounds* g_pDepthBoundsTest = NULL;
	#endif
#endif

// Only needed to load AMD_AGS
#undef LoadLibrary

#if CRY_PLATFORM_WINDOWS
// Count monitors helper
static BOOL CALLBACK CountConnectedMonitors(HMONITOR hMonitor, HDC hDC, LPRECT pRect, LPARAM opaque)
{
	uint* count = reinterpret_cast<uint*>(opaque);
	(*count)++;
	return TRUE;
}

#endif

void CD3D9Renderer::DisplaySplash()
{
#if CRY_PLATFORM_WINDOWS
	if (IsEditorMode())
		return;

	HBITMAP hImage = (HBITMAP)LoadImage(CryGetCurrentModule(), "splash.bmp", IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);

	HWND hwnd = (HWND)m_hWnd;
	if (hImage != INVALID_HANDLE_VALUE)
	{
		RECT rect;
		HDC hDC = GetDC(hwnd);
		HDC hDCBitmap = CreateCompatibleDC(hDC);
		BITMAP bm;

		GetClientRect(hwnd, &rect);
		GetObjectA(hImage, sizeof(bm), &bm);
		SelectObject(hDCBitmap, hImage);

		RECT Rect;
		GetWindowRect(hwnd, &Rect);
		StretchBlt(hDC, 0, 0, Rect.right - Rect.left, Rect.bottom - Rect.top, hDCBitmap, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);

		DeleteObject(hImage);
		DeleteDC(hDCBitmap);
	}
#endif
}

bool CD3D9Renderer::ChangeRenderResolution(int nNewRenderWidth, int nNewRenderHeight, CRenderView* pRenderView)
{
	CRY_ASSERT(!m_pRT || m_pRT->IsRenderThread());

	pRenderView->ChangeRenderResolution(nNewRenderWidth, nNewRenderHeight, false);
	pRenderView->SetViewport(SRenderViewport(0, 0, nNewRenderWidth, nNewRenderHeight));

	return true;
}

bool CD3D9Renderer::ChangeOutputResolution(int nNewOutputWidth, int nNewOutputHeight, CRenderOutput* pRenderOutput)
{
	CRY_ASSERT(!m_pRT || m_pRT->IsRenderThread());

	pRenderOutput->ChangeOutputResolution(nNewOutputWidth, nNewOutputHeight);
	pRenderOutput->SetViewport(SRenderViewport(0, 0, nNewOutputWidth, nNewOutputHeight));

	return true;
}

bool CD3D9Renderer::ChangeDisplayResolution(int nNewDisplayWidth, int nNewDisplayHeight, int nNewColDepth, int nNewRefreshHZ, bool bForceReset, CRenderDisplayContext* pDC)
{
	CSwapChainBackedRenderDisplayContext* pBC = GetBaseDisplayContext();

	iLog->Log("Changing resolution...");

	m_isChangingResolution = true;

	if (nNewColDepth < 24)
		nNewColDepth = 16;
	else
		nNewColDepth = 32;

	// Save the new dimensions
	m_cbpp = nNewColDepth;
#if defined(SUPPORT_DEVICE_INFO_USER_DISPLAY_OVERRIDES)
	m_overrideRefreshRate = CV_r_overrideRefreshRate;
	m_overrideScanlineOrder = CV_r_overrideScanlineOrder;
#endif

	const bool isFullscreen = IsFullscreen();
	if (isFullscreen && nNewColDepth == 16)
	{
		m_zbpp = 16;
		m_sbpp = 0;
	}

	RestoreGamma();

	CRY_ASSERT(!IsEditorMode() || bForceReset);

	if (!IsEditorMode() && (pDC == pBC))
	{
		// This is only allowed for the main viewport
		CRY_ASSERT(pBC->IsMainViewport());

		GetS3DRend().ReleaseBuffers();

#if defined(SUPPORT_DEVICE_INFO)
#if CRY_PLATFORM_WINDOWS
		// disable floating point exceptions due to driver bug when switching to fullscreen
		SCOPED_DISABLE_FLOAT_EXCEPTIONS();
#endif

		pBC->ChangeOutputIfNecessary(isFullscreen, IsVSynced());

#if DURANGO_ENABLE_ASYNC_DIPS
		WaitForAsynchronousDevice();
#endif

// 		OnD3D11PostCreateDevice(m_devInfo.Device());

		if (isFullscreen)
		{
			// Leave fullscreen before resizing as SetFullscreenState doesn't
			// resize the swapchain unless the fullscreen state changes
			pBC->SetFullscreenState(!isFullscreen);
		}

		pBC->ChangeDisplayResolution(nNewDisplayWidth, nNewDisplayHeight);
		pBC->SetFullscreenState(isFullscreen);

#endif
	}
	else
	{
		pDC->ChangeDisplayResolution(nNewDisplayWidth, nNewDisplayHeight);
		if (pDC->IsSwapChainBacked())
			static_cast<CSwapChainBackedRenderDisplayContext*>(pDC)->SetFullscreenState(isFullscreen);
	}

	PostDeviceReset();

	m_isChangingResolution = false;

	return true;
}

bool CD3D9Renderer::ChangeDisplayResolution(int nNewDisplayWidth, int nNewDisplayHeight, int nNewColDepth, int nNewRefreshHZ, bool bForceReset, const SDisplayContextKey& displayContextKey)
{
	CRY_ASSERT(!m_pRT || m_pRT->IsRenderThread());

	CRenderDisplayContext* pBC = GetBaseDisplayContext();
	CRenderDisplayContext* pDC = pBC;
	if (displayContextKey != SDisplayContextKey{})
		pDC = FindDisplayContext(displayContextKey);

	return ChangeDisplayResolution(nNewDisplayWidth, nNewDisplayHeight, nNewColDepth, nNewRefreshHZ, bForceReset, pDC);
}

void CD3D9Renderer::PostDeviceReset()
{
	if (IsFullscreen())
		SetGamma(CV_r_gamma + m_fDeltaGamma, CV_r_brightness, CV_r_contrast, true);

	m_nFrameReset++;
}

//-----------------------------------------------------------------------------
// Name: CD3D9Renderer::AdjustWindowForChange()
// Desc: Prepare the window for a possible change between windowed mode and
//       fullscreen mode.  This function is virtual and thus can be overridden
//       to provide different behavior, such as switching to an entirely
//       different window for fullscreen mode (as in the MFC sample apps).
//-----------------------------------------------------------------------------
HRESULT CD3D9Renderer::ChangeWindowProperties(const int displayWidth, const int displayHeight)
{
#if CRY_PLATFORM_WINDOWS || CRY_RENDERER_OPENGL
	if (IsEditorMode())
		return S_OK;
#endif

#if CRY_RENDERER_OPENGL
	CSwapChainBackedRenderDisplayContext* pDC = GetBaseDisplayContext();
	const DXGI_SWAP_CHAIN_DESC& swapChainDesc(m_devInfo.SwapChainDesc());

	DXGI_MODE_DESC modeDesc;
	modeDesc.Width  = displayWidth;
	modeDesc.Height = displayHeight;
	modeDesc.RefreshRate = swapChainDesc.BufferDesc.RefreshRate;
	modeDesc.Format = swapChainDesc.BufferDesc.Format;
	modeDesc.ScanlineOrdering = swapChainDesc.BufferDesc.ScanlineOrdering;
	modeDesc.Scaling = swapChainDesc.BufferDesc.Scaling;

	HRESULT result = pDC->GetSwapChain()->ResizeTarget(&modeDesc);
	if (FAILED(result))
		return result;
#elif CRY_PLATFORM_WINDOWS
	CSwapChainBackedRenderDisplayContext* pDC = GetBaseDisplayContext();

	HWND hwnd = (HWND)m_hWnd;
	if (IsFullscreen())
	{
		if (m_lastWindowState != EWindowState::Fullscreen)
		{
			constexpr auto fullscreenStyle = WS_POPUP | WS_VISIBLE;
			SetWindowLongPtrW(hwnd, GWL_STYLE, fullscreenStyle);
		}

		SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, displayWidth, displayHeight, SWP_SHOWWINDOW);
	}
	else if (m_currWindowState == EWindowState::BorderlessWindow || m_currWindowState == EWindowState::BorderlessFullscreen)
	{
		RectI monitorBounds = pDC->GetCurrentMonitorBounds();

		if (m_lastWindowState != EWindowState::BorderlessWindow)
		{
			// Set fullscreen-mode style
			constexpr auto fullscreenWindowStyle = WS_POPUP | WS_VISIBLE;
			SetWindowLongPtrW(hwnd, GWL_STYLE, fullscreenWindowStyle);
		}

		const int x = monitorBounds.x + (monitorBounds.w - displayWidth) / 2;
		const int y = monitorBounds.y + (monitorBounds.h - displayHeight) / 2;
		SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, displayWidth, displayHeight, SWP_SHOWWINDOW);
	}
	else
	{
		auto windowedStyle = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
		if (CV_r_ResizableWindow && CV_r_ResizableWindow->GetIVal() == 0)
		{
			windowedStyle &= ~(WS_MAXIMIZEBOX | WS_THICKFRAME);
		}

		SetWindowLongPtrW(hwnd, GWL_STYLE, windowedStyle);

		RECT windowRect;
		GetWindowRect(hwnd, &windowRect);

		const int x = windowRect.left;
		const int y = windowRect.top;

		windowRect.right = windowRect.left + displayWidth;
		windowRect.bottom = windowRect.top + displayHeight;
		AdjustWindowRectEx(&windowRect, windowedStyle, FALSE, WS_EX_APPWINDOW);

		const int width = windowRect.right - windowRect.left;
		const int height = windowRect.bottom - windowRect.top;
		SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, width, height, SWP_SHOWWINDOW);
	}
#endif

	return S_OK;
}

#if defined(SUPPORT_DEVICE_INFO)
bool compareDXGIMODEDESC(const DXGI_MODE_DESC& lhs, const DXGI_MODE_DESC& rhs)
{
	if (lhs.Width != rhs.Width)
		return lhs.Width < rhs.Width;
	return lhs.Height < rhs.Height;
}
#endif

int CD3D9Renderer::EnumDisplayFormats(SDispFormat* formats)
{
#if CRY_PLATFORM_WINDOWS || CRY_RENDERER_OPENGL

	#if defined(SUPPORT_DEVICE_INFO)

	auto* pDC = gcpRendD3D->GetActiveDisplayContext();
	auto* swapDC = pDC->IsSwapChainBacked() ? static_cast<CSwapChainBackedRenderDisplayContext*>(pDC) : gcpRendD3D->GetBaseDisplayContext();
	const DXGI_SURFACE_DESC& swapChainDesc = swapDC->GetSwapChain().GetSurfaceDesc();

	unsigned int numModes = 0;
	if (SUCCEEDED(swapDC->m_pOutput->GetDisplayModeList(swapChainDesc.Format, 0, &numModes, 0)) && numModes)
	{
		std::vector<DXGI_MODE_DESC> dispModes(numModes);
		if (SUCCEEDED(swapDC->m_pOutput->GetDisplayModeList(swapChainDesc.Format, 0, &numModes, &dispModes[0])) && numModes)
		{

			std::sort(dispModes.begin(), dispModes.end(), compareDXGIMODEDESC);

			unsigned int numUniqueModes = 0;
			unsigned int prevWidth = 0;
			unsigned int prevHeight = 0;
			for (unsigned int i = 0; i < numModes; ++i)
			{
				if (prevWidth != dispModes[i].Width || prevHeight != dispModes[i].Height)
				{
					if (formats)
					{
						formats[numUniqueModes].m_Width = dispModes[i].Width;
						formats[numUniqueModes].m_Height = dispModes[i].Height;
						formats[numUniqueModes].m_BPP = CTexture::BitsPerPixel(DeviceFormats::ConvertToTexFormat(dispModes[i].Format));
					}

					prevWidth = dispModes[i].Width;
					prevHeight = dispModes[i].Height;
					++numUniqueModes;
				}
			}

			numModes = numUniqueModes;
		}
	}

	return numModes;

	#endif

#else
	return 0;
#endif
}

void CD3D9Renderer::UnSetRes()
{
	m_Features |= RFT_SUPPORTZBIAS;

#if defined(DX11_ALLOW_D3D_DEBUG_RUNTIME)
	m_d3dDebug.Release();
#endif
}

void CD3D9Renderer::DestroyWindow(void)
{
#if defined(USE_SDL2_VIDEO) && (CRY_RENDERER_OPENGL || CRY_RENDERER_OPENGLES)
	DXGLDestroySDLWindow(m_hWnd);
#elif defined(USE_SDL2_VIDEO) && (CRY_RENDERER_VULKAN)
	VKDestroySDLWindow(m_hWnd);
#elif CRY_PLATFORM_WINDOWS
	if (gEnv && gEnv->pSystem && !m_bShaderCacheGen)
	{
		GetISystem()->GetISystemEventDispatcher()->RemoveListener(this);
	}
	if (m_hWnd)
	{
		::DestroyWindow((HWND)m_hWnd);
		m_hWnd = NULL;
	}
	if (m_hIconBig)
	{
		::DestroyIcon(m_hIconBig);
		m_hIconBig = NULL;
	}
	if (m_hIconSmall)
	{
		::DestroyIcon(m_hIconSmall);
		m_hIconSmall = NULL;
	}
	if (m_hCursor)
	{
		::DestroyCursor(m_hCursor);
		m_hCursor = NULL;
	}
#endif
}

static CD3D9Renderer::SGammaRamp orgGamma;

#if CRY_PLATFORM_WINDOWS
static BOOL g_doGamma = false;
#endif

void CD3D9Renderer::RestoreGamma(void)
{
	//if (!m_bFullScreen)
	//	return;

	if (!(GetFeatures() & RFT_HWGAMMA))
		return;

	if ((CV_r_nohwgamma == 1) || (CV_r_nohwgamma == 2 && IsEditorMode()))
		return;

	m_fLastGamma = 1.0f;
	m_fLastBrightness = 0.5f;
	m_fLastContrast = 0.5f;

	//iLog->Log("...RestoreGamma");

#if CRY_PLATFORM_WINDOWS
	if (!g_doGamma)
		return;

	g_doGamma = false;

	m_hWndDesktop = GetDesktopWindow();

	if (HDC dc = GetDC((HWND)m_hWndDesktop))
	{
		SetDeviceGammaRamp(dc, &orgGamma);
		ReleaseDC((HWND)m_hWndDesktop, dc);
	}
#endif
}

void CD3D9Renderer::GetDeviceGamma()
{
#if CRY_PLATFORM_WINDOWS
	if (g_doGamma)
	{
		return;
	}

	m_hWndDesktop = GetDesktopWindow();

	if (HDC dc = GetDC((HWND)m_hWndDesktop))
	{
		g_doGamma = true;

		if (!GetDeviceGammaRamp(dc, &orgGamma))
		{
			for (uint16 i = 0; i < 256; i++)
			{
				orgGamma.red[i] = i * 0x101;
				orgGamma.green[i] = i * 0x101;
				orgGamma.blue[i] = i * 0x101;
			}
		}

		ReleaseDC((HWND)m_hWndDesktop, dc);
	}
#endif
}

void CD3D9Renderer::SetDeviceGamma(SGammaRamp* gamma)
{
	if (!(GetFeatures() & RFT_HWGAMMA))
		return;

	if ((CV_r_nohwgamma == 1) || (CV_r_nohwgamma == 2 && IsEditorMode()))
		return;

#if CRY_PLATFORM_WINDOWS
	if (!g_doGamma)
		return;

	m_hWndDesktop = GetDesktopWindow();  // TODO: DesktopWindow - does not represent actual output window thus gamma affects all desktop monitors !!!

	if (HDC dc = GetDC((HWND)m_hWndDesktop))
	{
		g_doGamma = true;
		// INFO!!! - very strange: in the same time
		// GetDeviceGammaRamp -> TRUE
		// SetDeviceGammaRamp -> FALSE but WORKS!!!
		// at least for desktop window DC... be careful
		SetDeviceGammaRamp(dc, gamma);
		ReleaseDC((HWND)m_hWndDesktop, dc);
	}
#endif
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

void CD3D9Renderer::SetGamma(float fGamma, float fBrightness, float fContrast, bool bForce)
{
	if (m_pStereoRenderer)  // Function can be called on shutdown
		fGamma += GetS3DRend().GetGammaAdjustment();

	fGamma = CLAMP(fGamma, 0.4f, 1.6f);

	if (!bForce && m_fLastGamma == fGamma && m_fLastBrightness == fBrightness && m_fLastContrast == fContrast)
		return;

	GetDeviceGamma();

	SGammaRamp gamma;

	// Code kept for possible backward compatibility
	const bool bModifyOldTable = false;
	float fInvGamma = 1.f / fGamma;

	float fAdd = (fBrightness - 0.5f) * 0.5f - fContrast * 0.5f + 0.25f;
	float fMul = fContrast + 0.5f;

	for (int i = 0; i < 256; i++)
	{
		float pfInput[3];

		if (bModifyOldTable)
		{
			pfInput[0] = (float)(orgGamma.red  [i] >> 8) / 255.f;
			pfInput[1] = (float)(orgGamma.green[i] >> 8) / 255.f;
			pfInput[2] = (float)(orgGamma.blue [i] >> 8) / 255.f;
		}
		else
		{
			pfInput[0] = i / 255.f;
			pfInput[1] = i / 255.f;
			pfInput[2] = i / 255.f;
		}

		pfInput[0] = pow_tpl(pfInput[0], fInvGamma) * fMul + fAdd;
		pfInput[1] = pow_tpl(pfInput[1], fInvGamma) * fMul + fAdd;
		pfInput[2] = pow_tpl(pfInput[2], fInvGamma) * fMul + fAdd;

		gamma.red  [i] = CLAMP(int_round(pfInput[0] * 65535.f), 0, 65535);
		gamma.green[i] = CLAMP(int_round(pfInput[1] * 65535.f), 0, 65535);
		gamma.blue [i] = CLAMP(int_round(pfInput[2] * 65535.f), 0, 65535);
	}

	SetDeviceGamma(&gamma);

	m_fLastGamma = fGamma;
	m_fLastBrightness = fBrightness;
	m_fLastContrast = fContrast;
}

bool CD3D9Renderer::SetGammaDelta(const float fGamma)
{
	m_fDeltaGamma = fGamma;
	SetGamma(CV_r_gamma + fGamma, CV_r_brightness, CV_r_contrast, false);
	return true;
}

void CD3D9Renderer::ShutDownFast()
{
	SAFE_DELETE(m_pVRProjectionManager);

	// Flush RT command buffer
	ForceFlushRTCommands();
	CHWShader::mfFlushPendedShadersWait(-1);
	EF_Exit(true);
	//CBaseResource::ShutDown();

	SAFE_DELETE(m_pRT);

#if CRY_RENDERER_OPENGL
	#if !DXGL_FULL_EMULATION && !OGL_SINGLE_CONTEXT
	if (CV_r_multithreaded)
		DXGLReleaseContext(m_devInfo.Device());
	#endif
	m_devInfo.Release();
#endif
}

static CryCriticalSection gs_contextLock;

void CD3D9Renderer::RT_ShutDown(uint32 nFlags)
{
	if (m_pGpuParticleManager)
		static_cast<gpu_pfx2::CManager*>(m_pGpuParticleManager)->CleanupResources();


	CSceneRenderPass::Shutdown();
	m_particleBuffer.Release();

	CREBreakableGlassBuffer::RT_ReleaseInstance();
	SAFE_DELETE(m_pVRProjectionManager);
	SAFE_DELETE(m_pPostProcessMgr);
	SAFE_DELETE(m_pWaterSimMgr);
	SAFE_DELETE(m_pGpuParticleManager);
	SAFE_DELETE(m_pComputeSkinningStorage);
	SAFE_DELETE(m_pStereoRenderer);
	SAFE_DELETE(m_pPipelineProfiler);
#if defined(ENABLE_RENDER_AUX_GEOM)
	SAFE_DELETE(m_pRenderAuxGeomD3D);
#endif

	for (size_t i = 0; i < 3; ++i)
		while (m_CharCBActiveList[i].next != &m_CharCBActiveList[i])
			delete m_CharCBActiveList[i].next->item<&SCharacterInstanceCB::list>();
	while (m_CharCBFreeList.next != &m_CharCBFreeList)
		delete m_CharCBFreeList.next->item<&SCharacterInstanceCB::list>();

	CHWShader::mfFlushPendedShadersWait(-1);
	if (!IsShaderCacheGenMode())
	{
		if (nFlags == FRR_SHUTDOWN)
		{
			FreeSystemResources(nFlags);
		}
	}
	else
	{
		CREParticle::ResetPool();

		if (m_pStereoRenderer)
			m_pStereoRenderer->ReleaseBuffers();

#if defined(ENABLE_RENDER_AUX_GEOM)
		if (m_pRenderAuxGeomD3D)
			m_pRenderAuxGeomD3D->ReleaseResources();
#endif //ENABLE_RENDER_AUX_GEOM

		m_pBaseGraphicsPipeline.reset();
		m_pActiveGraphicsPipeline.reset();
		m_graphicsPipelines.clear();

#if defined(FEATURE_SVO_GI)
		// TODO: GraphicsPipeline-Stage shutdown with ShutDown()
		if (auto pSvoRenderer = CSvoRenderer::GetInstance(false))
			pSvoRenderer->Release();
#endif

		EF_Exit();

		ForceFlushRTCommands();
	}

	GetDeviceObjectFactory().ReleaseInputLayouts();
	GetDeviceObjectFactory().ReleaseSamplerStates();
	GetDeviceObjectFactory().ReleaseNullResources();

#if defined(SUPPORT_DEVICE_INFO)
	//m_devInfo.Release();
	#if CRY_RENDERER_OPENGL && !DXGL_FULL_EMULATION
		#if OGL_SINGLE_CONTEXT
	m_pRT->m_kDXGLDeviceContextHandle.Set(NULL);
		#else
	m_pRT->m_kDXGLDeviceContextHandle.Set(NULL, !CV_r_multithreaded);
	m_pRT->m_kDXGLContextHandle.Set(NULL);
		#endif
	#endif
#endif

	// Shut Down all contexts.
	{
		AUTO_LOCK(gs_contextLock); // Not thread safe without this
		for (auto& pDisplayContext : m_displayContexts)
			pDisplayContext.second->ReleaseResources();
	}

#if !CRY_PLATFORM_ORBIS && !CRY_RENDERER_OPENGL
	GetDeviceContext().ReleaseDeviceContext();
#endif

#if defined(ENABLE_NULL_D3D11DEVICE)
	if (m_bShaderCacheGen)
		GetDevice().ReleaseDevice();
#endif
}

void CD3D9Renderer::ShutDown(bool bReInit)
{
	m_bInShutdown = true;

	// Force Flush RT command buffer
	ForceFlushRTCommands();
	PreShutDown();

	DeleteAuxGeomCollectors();
	DeleteAuxGeomCBs();

	if (m_pRT)
	{
		ExecuteRenderThreadCommand([=] { this->RT_ShutDown(bReInit ? 0 : FRR_SHUTDOWN); }, ERenderCommandFlags::FlushAndWait);
	}

	//CBaseResource::ShutDown();
	ForceFlushRTCommands();

#ifdef USE_PIX_DURANGO
	SAFE_RELEASE(m_pPixPerf);
#endif

	//////////////////////////////////////////////////////////////////////////
	// Clear globals.
	//////////////////////////////////////////////////////////////////////////

	// Release Display Contexts, freeing Swap Channels.
	m_pBaseDisplayContext = nullptr;
	SetActiveContext(nullptr, {});

	{
		AUTO_LOCK(gs_contextLock); // Not thread safe without this
		m_displayContexts.clear();
	}

	SAFE_DELETE(m_pRT);

#if CRY_RENDERER_OPENGL && !DXGL_FULL_EMULATION && !OGL_SINGLE_CONTEXT
	if (CV_r_multithreaded)
		DXGLReleaseContext(GetDevice().GetRealDevice());
#endif

	if (!m_DevBufMan.Shutdown())
		CryWarning(VALIDATOR_MODULE_RENDERER, VALIDATOR_ERROR_DBGBRK, "could not free all buffers from CDevBufferMan!");

#if CRY_PLATFORM_IOS || CRY_PLATFORM_ANDROID || CRY_PLATFORM_WINDOWS || CRY_PLATFORM_APPLE || CRY_PLATFORM_LINUX
#if defined(SUPPORT_DEVICE_INFO)
	m_devInfo.Release();
#endif
#endif

	if (!bReInit)
	{
		iLog = NULL;
		//iConsole = NULL;
		iTimer = NULL;
		iSystem = NULL;
	}

	PostShutDown();
}

#if CRY_PLATFORM_WINDOWS
LRESULT CALLBACK LowLevelKeyboardProc(INT nCode, WPARAM wParam, LPARAM lParam)
{
	KBDLLHOOKSTRUCT* pkbhs = (KBDLLHOOKSTRUCT*) lParam;

	switch (nCode)
	{
	case HC_ACTION:
		{
			if (pkbhs->vkCode == VK_TAB && pkbhs->flags & LLKHF_ALTDOWN)
				return 1;            // Disable ALT+ESC
		}
	default:
		break;
	}
	return CallNextHookEx(0, nCode, wParam, lParam);
}
#endif

#if defined(SUPPORT_DEVICE_INFO)
CRY_HWND CD3D9Renderer::CreateWindowCallback()
{
	const CRenderDisplayContext* pDC = gcpRendD3D->GetBaseDisplayContext();

	const int displayWidth  = pDC->GetDisplayResolution()[0];
	const int displayHeight = pDC->GetDisplayResolution()[1];

	return gcpRendD3D->SetWindow(displayWidth, displayHeight) ? gcpRendD3D->m_hWnd : 0;
}
#endif

void CD3D9Renderer::InitBaseDisplayContext()
{
#if CRY_PLATFORM_WINDOWS || CRY_PLATFORM_ANDROID || CRY_PLATFORM_LINUX
	m_VSync = !IsEditorMode() ? CV_r_vsync : 0;

	// Update base context hWnd and key
	SDisplayContextKey baseContextKey;
	baseContextKey.key.emplace<CRY_HWND>(m_pBaseDisplayContext->GetWindowHandle());
	m_pBaseDisplayContext->CreateSwapChain(m_hWnd, IsFullscreen(), IsVSynced());

	{
		AUTO_LOCK(gs_contextLock);
		m_displayContexts.erase(baseContextKey);

		baseContextKey.key.emplace<CRY_HWND>(m_hWnd);
		m_displayContexts.emplace(std::make_pair(std::move(baseContextKey), m_pBaseDisplayContext));
	}
#endif
}

bool CD3D9Renderer::SetWindow(int width, int height)
{
	CRY_PROFILE_FUNCTION(PROFILE_LOADING_ONLY);

	GetISystem()->GetISystemEventDispatcher()->RegisterListener(this, "CD3DRenderer");

#if USE_SDL2_VIDEO && (CRY_RENDERER_OPENGL || CRY_RENDERER_OPENGLES)
	DXGLCreateSDLWindow(m_WinTitle, width, height, IsFullscreen(), &m_hWnd);
#elif USE_SDL2_VIDEO && (CRY_RENDERER_VULKAN)
	VKCreateSDLWindow(m_WinTitle, width, height, IsFullscreen(), &m_hWnd);
#elif CRY_PLATFORM_WINDOWS
	DWORD style, exstyle;

	if (width < 640)
		width = 640;
	if (height < 480)
		height = 480;

	MONITORINFO monitorInfo;
	monitorInfo.cbSize = sizeof(monitorInfo);
	GetMonitorInfo(MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY), &monitorInfo);
	int x = monitorInfo.rcMonitor.left;
	int y = monitorInfo.rcMonitor.top;
	const int monitorWidth = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
	const int monitorHeight = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;

	bool isFullscreenWindow = m_currWindowState == EWindowState::BorderlessWindow;

	if (IsFullscreen() || isFullscreenWindow)
	{
		exstyle = isFullscreenWindow ? WS_EX_APPWINDOW : WS_EX_TOPMOST;
		style = WS_POPUP | WS_VISIBLE;
		x += (monitorWidth - width) / 2;
		y += (monitorHeight - height) / 2;
	}
	else
	{
		exstyle = WS_EX_APPWINDOW;
		style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
		const ICVar* pResizeableWindow = gEnv->pConsole->GetCVar("r_resizableWindow");
		if (pResizeableWindow && pResizeableWindow->GetIVal() == 0)
		{
			style &= ~(WS_MAXIMIZEBOX | WS_THICKFRAME);
		}

		RECT wndrect;
		SetRect(&wndrect, 0, 0, width, height);
		AdjustWindowRectEx(&wndrect, style, FALSE, exstyle);

		width = wndrect.right - wndrect.left;
		height = wndrect.bottom - wndrect.top;

		x += (monitorWidth - width) / 2;
		y += (monitorHeight - height) / 2;
	}

	if (IsEditorMode())
	{
	#if defined(UNICODE) || defined(_UNICODE)
		#error Review this, probably should be wide if Editor also has UNICODE support (or maybe moved into Editor)
	#endif
		style = WS_OVERLAPPED;
		exstyle = 0;
		x = y = 0;
		width = 100;
		height = 100;

		WNDCLASSA wc;
		memset(&wc, 0, sizeof(WNDCLASSA));
		wc.style = CS_OWNDC;
		wc.lpfnWndProc = DefWindowProcA;
		wc.hInstance = CryGetCurrentModule();
		wc.lpszClassName = "D3DDeviceWindowClassForSandbox";
		if (!RegisterClassA(&wc))
		{
			CryFatalError("Cannot Register Window Class %s", wc.lpszClassName);
			return false;
		}
		m_hWnd = CreateWindowExA(exstyle, wc.lpszClassName, m_WinTitle, style, x, y, width, height, NULL, NULL, wc.hInstance, NULL);
		ShowWindow((HWND)m_hWnd, SW_HIDE);
	}
	else
	{
		if (!m_hWnd)
		{
			LPCWSTR pClassName = L"CryENGINE";

			// Load default icon if we have nothing yet
			if (m_hIconBig == NULL)
			{
				SetWindowIcon(gEnv->pConsole->GetCVar("r_WindowIconTexture")->GetString());
			}

			if (m_hCursor == NULL && gEnv->pConsole->GetCVar("r_MouseUseSystemCursor")->GetIVal() != 0)
			{
				const char* texture = gEnv->pConsole->GetCVar("r_MouseCursorTexture")->GetString();
				if (texture && *texture)
				{
					m_hCursor = CreateResourceFromTexture(this, texture, eResourceType_Cursor);
				}
				else
				{
					m_hCursor = ::LoadCursor(NULL, IDC_ARROW);
				}
			}

			// Moved from Game DLL
			WNDCLASSEXW wc;
			memset(&wc, 0, sizeof(WNDCLASSEXW));
			wc.cbSize = sizeof(WNDCLASSEXW);
			wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
			wc.lpfnWndProc = (WNDPROC)GetISystem()->GetRootWindowMessageHandler();
			wc.hInstance = CryGetCurrentModule();
			wc.hIcon = m_hIconBig;
			wc.hIconSm = m_hIconSmall;
			wc.hCursor = m_hCursor;
			wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
			wc.lpszClassName = pClassName;
			if (!RegisterClassExW(&wc))
			{
				CryFatalError("Cannot Register Launcher Window Class");
				return false;
			}

			wstring wideTitle = Unicode::Convert<wstring>(m_WinTitle);

			m_hWnd = CreateWindowExW(exstyle, pClassName, wideTitle.c_str(), style, x, y, width, height, NULL, NULL, wc.hInstance, NULL);
			if (m_hWnd && !IsWindowUnicode((HWND)m_hWnd))
			{
				CryFatalError("Expected an UNICODE window for launcher");
				return false;
			}

			EnableCloseButton(m_hWnd, false);

			if (IsFullscreen() && (!gEnv->pSystem->IsDevMode() && CV_r_enableAltTab == 0))
				SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
		}

		HWND hwnd = (HWND)m_hWnd;
		if (hwnd)
		{
			ShowWindow(hwnd, SW_SHOWNORMAL);
			const bool wasFocusSet = SetFocus(hwnd) != nullptr;
			CRY_ASSERT(wasFocusSet);
			// Attempt to move the window to the foreground and activate it
			// Note that this will fail if the user alt-tabbed away to another application while the engine was starting
			// See https://msdn.microsoft.com/en-us/library/windows/desktop/ms633539(v=vs.85).aspx
			SetForegroundWindow(hwnd);
		}
	}
#endif

	if (!m_hWnd)
		iConsole->Exit("Couldn't create window\n");

#if CRY_PLATFORM_WINDOWS
	if (m_hWnd)
	{
		m_changedMonitor = true;
		m_activeMonitor = nullptr;
	}
#endif

	return m_hWnd != nullptr;

}

bool CD3D9Renderer::SetWindowIcon(const char* path)
{
#if CRY_PLATFORM_WINDOWS
	if (IsEditorMode())
	{
		return false;
	}

	if (stricmp(path, m_iconPath.c_str()) == 0)
	{
		return true;
	}

	HICON hIconBig = CreateResourceFromTexture(this, path, eResourceType_IconBig);
	if (hIconBig)
	{
		if (m_hWnd)
		{
			SendMessage((HWND)m_hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
		}
		if (m_hIconBig)
		{
			::DestroyIcon(m_hIconBig);
		}
		m_hIconBig = hIconBig;
		m_iconPath = path;
	}

	// Note: Also set the small icon manually.
	// Even though the big icon will also affect the small icon, the re-scaling done by GDI has aliasing problems.
	// Just grabbing a smaller MIP from the texture (if possible) will solve this.
	HICON hIconSmall = CreateResourceFromTexture(this, path, eResourceType_IconSmall);
	if (hIconSmall)
	{
		if (m_hWnd)
		{
			SendMessage((HWND)m_hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
		}
		if (m_hWnd)
		{
			SendMessage((HWND)m_hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
		}
		if (m_hIconSmall)
		{
			::DestroyIcon(m_hIconSmall);
		}
		m_hIconSmall = hIconSmall;
	}

	// Update the CVar value to match, in case this API was not called through CVar-change-callback
	gEnv->pConsole->GetCVar("r_WindowIconTexture")->Set(m_iconPath.c_str());

	return hIconBig != NULL;
#else
	return false;
#endif
}

#define QUALITY_VAR(name)                                                 \
  static void OnQShaderChange_Shader ## name(ICVar * pVar)                \
  {                                                                       \
    int iQuality = eSQ_Low;                                               \
    if (gRenDev->GetFeatures() & (RFT_HW_SM40))                           \
      iQuality = CLAMP(pVar->GetIVal(), 0, eSQ_Max);                      \
    gRenDev->EF_SetShaderQuality(eST_ ## name, (EShaderQuality)iQuality); \
  }

QUALITY_VAR(General)
QUALITY_VAR(Metal)
QUALITY_VAR(Glass)
QUALITY_VAR(Vegetation)
QUALITY_VAR(Ice)
QUALITY_VAR(Terrain)
QUALITY_VAR(Shadow)
QUALITY_VAR(Water)
QUALITY_VAR(FX)
QUALITY_VAR(PostProcess)
QUALITY_VAR(HDR)

#undef QUALITY_VAR

static void OnQShaderChange_Renderer(ICVar* pVar)
{
	int iQuality = eRQ_Low;

	if (gRenDev->GetFeatures() & (RFT_HW_SM40))
		iQuality = CLAMP(pVar->GetIVal(), 0, eSQ_Max);
	else
		pVar->ForceSet("0");

	auto rq = gRenDev->GetRenderQuality();
	rq.renderQuality = (ERenderQuality)iQuality;
	gRenDev->SetRenderQuality(rq);
}

static void Command_Quality(IConsoleCmdArgs* Cmd)
{
	bool bLog = false;
	bool bSet = false;

	int iQuality = -1;

	if (Cmd->GetArgCount() == 2)
	{
		iQuality = CLAMP(atoi(Cmd->GetArg(1)), 0, eSQ_Max);
		bSet = true;
	}
	else bLog = true;

	if (bLog) iLog->LogWithType(IMiniLog::eInputResponse, " ");
	if (bLog) iLog->LogWithType(IMiniLog::eInputResponse, "Current quality settings (0=low/1=med/2=high/3=very high):");

#define QUALITY_VAR(name) if (bLog) iLog->LogWithType(IMiniLog::eInputResponse, "  $3q_" # name " = $6%d", gEnv->pConsole->GetCVar("q_" # name)->GetIVal()); \
  if (bSet) gEnv->pConsole->GetCVar("q_" # name)->Set(iQuality);

	QUALITY_VAR(ShaderGeneral)
	QUALITY_VAR(ShaderMetal)
	QUALITY_VAR(ShaderGlass)
	QUALITY_VAR(ShaderVegetation)
	QUALITY_VAR(ShaderIce)
	QUALITY_VAR(ShaderTerrain)
	QUALITY_VAR(ShaderShadow)
	QUALITY_VAR(ShaderWater)
	QUALITY_VAR(ShaderFX)
	QUALITY_VAR(ShaderPostProcess)
	QUALITY_VAR(ShaderHDR)
	QUALITY_VAR(ShaderSky)
	QUALITY_VAR(Renderer)

#undef QUALITY_VAR

	if (bSet) iLog->LogWithType(IMiniLog::eInputResponse, "Set quality to %d", iQuality);
}

const char* sGetSQuality(const char* szName)
{
	ICVar* pVar = iConsole->GetCVar(szName);
	assert(pVar);
	if (!pVar)
		return "Unknown";
	int iQ = pVar->GetIVal();
	switch (iQ)
	{
	case eSQ_Low:
		return "Low";
	case eSQ_Medium:
		return "Medium";
	case eSQ_High:
		return "High";
	case eSQ_VeryHigh:
		return "VeryHigh";
	default:
		return "Unknown";
	}
}

CRY_HWND CD3D9Renderer::Init(int x, int y, int width, int height, unsigned int colorBits, int depthBits, int stencilBits, CRY_HWND Glhwnd, bool bReInit, bool bShaderCacheGen)
{
	CRY_PROFILE_FUNCTION(PROFILE_LOADING_ONLY);

	// Create and set the current aux collector to capture any aux commands
	SetCurrentAuxGeomCollector(GetOrCreateAuxGeomCollector(gEnv->pSystem->GetViewCamera()));

	MEMSTAT_CONTEXT(EMemStatContextType::Other, "Renderer initialisation");

	if (!iSystem || !iLog)
		return 0;

	iLog->Log("Initializing Direct3D and creating game window:");
	INDENT_LOG_DURING_SCOPE();

#if CRY_PLATFORM_WINDOWS
	REGISTER_STRING_CB("r_WindowIconTexture", "%ENGINE%/EngineAssets/Textures/default_icon.dds", VF_CHEAT | VF_CHEAT_NOCHECK,
	                   "Sets the image (dds file) to be displayed as the window and taskbar icon",
	                   SetWindowIconCVar);
#endif

	m_currWindowState = CalculateWindowState();

#if (CRY_RENDERER_OPENGL || CRY_RENDERER_OPENGLES) && !DXGL_FULL_EMULATION
	#if OGL_SINGLE_CONTEXT
	DXGLInitialize(0);
	#else
	DXGLInitialize(CV_r_multithreaded ? 4 : 0);
	#endif
#endif //CRY_RENDERER_OPENGL && !DXGL_FULL_EMULATION

	iLog->Log("Direct3D driver is creating...");
	iLog->Log("Crytek Direct3D driver version %4.2f (%s <%s>)", VERSION_D3D, __DATE__, __TIME__);

	const char* sGameName = gEnv->pSystem->GetIProjectManager()->GetCurrentProjectName();
	cry_strcpy(m_WinTitle, sGameName);

	iLog->Log("Creating window called '%s' (%dx%d)", m_WinTitle, width, height);

	if (Glhwnd == (CRY_HWND)1)
	{
		Glhwnd = 0;
		m_bEditor = true;

		if (m_currWindowState == EWindowState::Fullscreen)
			m_currWindowState = EWindowState::Windowed;
	}

	m_bShaderCacheGen = bShaderCacheGen;

	// RenderPipeline parameters
	int renderWidth, renderHeight;
	int outputWidth, outputHeight;
	int displayWidth, displayHeight;

	m_cbpp = colorBits;
	m_zbpp = depthBits;
	m_sbpp = stencilBits;

	CRenderDisplayContext* pDC = GetBaseDisplayContext();
	CalculateResolutions(width, height, &renderWidth, &renderHeight, &outputWidth, &outputHeight, &displayWidth, &displayHeight);

	pDC->m_DisplayWidth =  displayWidth;
	pDC->m_DisplayHeight = displayHeight;

	// only create device if we are not in shader cache generation mode
	if (!m_bShaderCacheGen)
	{
		// call init stereo before device is created!
		m_pStereoRenderer->InitDeviceBeforeD3D();

		while (true)
		{
			m_hWnd = (HWND)Glhwnd;

			// Creates Device here.
			bool bRes = m_pRT->RC_CreateDevice();
			if (!bRes)
			{
				CryWarning(VALIDATOR_MODULE_RENDERER, VALIDATOR_ERROR, "Rendering device creation failed!");
				ShutDown(true);
				return 0;
			}

			break;
		}

#if defined(SUPPORT_DEVICE_INFO)
		iLog->Log(" ****** CryRender InfoDump ******");
		iLog->Log(" Driver description: %S", m_devInfo.AdapterDesc().Description);
#endif

#if CRY_RENDERER_DIRECT3D
		iLog->Log(" API Version: Direct3D %d.%1d", (CRY_RENDERER_DIRECT3D / 10), (CRY_RENDERER_DIRECT3D % 10));
#endif

#if defined(SUPPORT_DEVICE_INFO)
		switch (m_devInfo.FeatureLevel())
		{
		case D3D_FEATURE_LEVEL_9_1:
			iLog->Log(" Feature level: D3D 9_1");
			iLog->Log(" Vertex Shaders version %d.%d", 2, 0);
			iLog->Log(" Pixel Shaders version %d.%d", 2, 0);
			break;
		case D3D_FEATURE_LEVEL_9_2:
			iLog->Log(" Feature level: D3D 9_2");
			iLog->Log(" Vertex Shaders version %d.%d", 2, 0);
			iLog->Log(" Pixel Shaders version %d.%d", 2, 0);
			break;
		case D3D_FEATURE_LEVEL_9_3:
			iLog->Log(" Feature level: D3D 9_3");
			iLog->Log(" Vertex Shaders version %d.%d", 3, 0);
			iLog->Log(" Pixel Shaders version %d.%d", 3, 0);
			break;
		case D3D_FEATURE_LEVEL_10_0:
			iLog->Log(" Feature level: D3D 10_0");
			iLog->Log(" Vertex Shaders version %d.%d", 4, 0);
			iLog->Log(" Pixel Shaders version %d.%d", 4, 0);
			break;
		case D3D_FEATURE_LEVEL_10_1:
			iLog->Log(" Feature level: D3D 10_1");
			iLog->Log(" Vertex Shaders version %d.%d", 4, 0);
			iLog->Log(" Pixel Shaders version %d.%d", 4, 0);
			break;
		case D3D_FEATURE_LEVEL_11_0:
			iLog->Log(" Feature level: D3D 11_0");
			iLog->Log(" Vertex Shaders version %d.%d", 5, 0);
			iLog->Log(" Pixel Shaders version %d.%d", 5, 0);
			break;
		case D3D_FEATURE_LEVEL_11_1:
			iLog->Log(" Feature level: D3D 11_1");
			iLog->Log(" Vertex Shaders version %d.%d", 5, 1);
			iLog->Log(" Pixel Shaders version %d.%d", 5, 1);
			break;
		case D3D_FEATURE_LEVEL_12_0:
			iLog->Log(" Feature level: D3D 12_0");
			iLog->Log(" Vertex Shaders version %d.%d", 6, 0);
			iLog->Log(" Pixel Shaders version %d.%d", 6, 0);
			break;
		case D3D_FEATURE_LEVEL_12_1:
			iLog->Log(" Feature level: D3D 12_1");
			iLog->Log(" Vertex Shaders version %d.%d", 6, 2);
			iLog->Log(" Pixel Shaders version %d.%d", 6, 2);
			break;
		default:
			iLog->Log(" Feature level: Unknown");
			break;
		}

		iLog->Log(" Full stats: %s", m_strDeviceStats);
		if (m_devInfo.DriverType() == D3D_DRIVER_TYPE_HARDWARE)
			iLog->Log(" Rasterizer: Hardware");
		else if (m_devInfo.DriverType() == D3D_DRIVER_TYPE_REFERENCE)
			iLog->Log(" Rasterizer: Reference");
		else if (m_devInfo.DriverType() == D3D_DRIVER_TYPE_SOFTWARE)
			iLog->Log(" Rasterizer: Software");
#elif CRY_PLATFORM_DURANGO
		iLog->Log(" Feature level: D3D 11_1");
		iLog->Log(" Vertex Shaders version %d.%d", 5, 0);
		iLog->Log(" Pixel Shaders version %d.%d", 5, 0);
#endif

		iLog->Log(" Current Resolution: %dx%dx%d %s", CRendererResources::s_renderWidth, CRendererResources::s_renderHeight, CRenderer::m_cbpp, GetWindowStateName());
		iLog->Log(" Occlusion queries: %s", (m_Features & RFT_OCCLUSIONTEST) ? "Supported" : "Not supported");
		iLog->Log(" Geometry instancing: %s", (m_bDeviceSupportsInstancing) ? "Supported" : "Not supported");
		iLog->Log(" NormalMaps compression : %s", CRendererResources::s_hwTexFormatSupport.m_FormatBC5U.IsValid() ? "Supported" : "Not supported");
		iLog->Log(" Gamma control: %s", (m_Features & RFT_HWGAMMA) ? "Hardware" : "Software");

		CRenderer::OnChange_GeomInstancingThreshold(0);   // to get log printout and to set the internal value (vendor dependent)

		m_Features |= RFT_HW_SM40;

		if (!m_bDeviceSupportsInstancing)
			_SetVar("r_GeomInstancing", 0);

		const char* str = NULL;
		if (m_Features & RFT_HW_SM62)
			str = "SM.6.2";
		else if (m_Features & RFT_HW_SM60)
			str = "SM.6.0";
		else if (m_Features & RFT_HW_SM51)
			str = "SM.5.1";
		else if (m_Features & RFT_HW_SM50)
			str = "SM.5.0";
		else if (m_Features & RFT_HW_SM40)
			str = "SM.4.0";
		else
			assert(0);
		iLog->Log(" Shader model usage: '%s'", str);

		m_pVRProjectionManager = new CVrProjectionManager(this);
		m_pVRProjectionManager->Init();

		CRendererResources::OnDisplayResolutionChanged(displayWidth, displayHeight);
		CRendererResources::OnOutputResolutionChanged(outputWidth, outputHeight);
		CRendererResources::OnRenderResolutionChanged(renderWidth, renderHeight);
	}
	else
	{

		// force certain features during shader cache gen mode
		m_Features |= RFT_HW_SM40;

#if defined(ENABLE_NULL_D3D11DEVICE)
		m_DeviceWrapper.AssignDevice(new NullD3D11Device);
		D3DDeviceContext* pContext = NULL;
	#if (CRY_RENDERER_DIRECT3D >= 113)
		GetDevice().GetImmediateContext3(&pContext);
	#elif (CRY_RENDERER_DIRECT3D >= 112)
		GetDevice().GetImmediateContext2(&pContext);
	#elif (CRY_RENDERER_DIRECT3D >= 111)
		GetDevice().GetImmediateContext1(&pContext);
	#else
		GetDevice().GetImmediateContext(&pContext);
	#endif
		m_DeviceContextWrapper.AssignDeviceContext(pContext);
#endif
	}

	iLog->Log(" *****************************************");
	iLog->Log(" ");

	iLog->Log("Init Shaders");

	//  if (!(GetFeatures() & (RFT_HW_PS2X | RFT_HW_PS30)))
	//    SetShaderQuality(eST_All, eSQ_Low);

	// Quality console variables --------------------------------------

#define QUALITY_VAR(name) { ICVar* pVar = ConsoleRegistrationHelper::Register("q_Shader" # name, &m_cEF.m_ShaderProfiles[(int)eST_ ## name].m_iShaderProfileQuality, 1, \
  0, CVARHELP("Defines the shader quality of " # name "\nUsage: q_Shader" # name " 0=low/1=med/2=high/3=very high (default)"), OnQShaderChange_Shader ## name);         \
OnQShaderChange_Shader## name(pVar);                                                                                                                                                                                   \
iLog->Log(" %s shader quality: %s", # name, sGetSQuality("q_Shader" # name)); } // clamp for lowspec

	QUALITY_VAR(General);
	QUALITY_VAR(Metal);
	QUALITY_VAR(Glass);
	QUALITY_VAR(Vegetation);
	QUALITY_VAR(Ice);
	QUALITY_VAR(Terrain);
	QUALITY_VAR(Shadow);
	QUALITY_VAR(Water);
	QUALITY_VAR(FX);
	QUALITY_VAR(PostProcess);
	QUALITY_VAR(HDR);

#undef QUALITY_VAR

	ICVar* pVar = REGISTER_INT_CB("q_Renderer", 3, 0, "Defines the quality of Renderer\nUsage: q_Renderer 0=low/1=med/2=high/3=very high (default)", OnQShaderChange_Renderer);
	OnQShaderChange_Renderer(pVar);   // clamp for lowspec, report renderer current value
	iLog->Log("Render quality: %s", sGetSQuality("q_Renderer"));

	REGISTER_COMMAND("q_Quality", &Command_Quality, 0,
	                 "If called with a parameter it sets the quality of all q_.. variables\n"
	                 "otherwise it prints their current state\n"
	                 "Usage: q_Quality [0=low/1=med/2=high/3=very high]");

#if defined(DURANGO_VSGD_CAP)
	REGISTER_COMMAND("GPUCapture", &GPUCapture, VF_NULL,
	                 "Usage: GPUCapture name"
	                 "Takes a PIX GPU capture with the specified name\n");
#endif

#if CRY_RENDERER_OPENGL && !DXGL_FULL_EMULATION
	#if OGL_SINGLE_CONTEXT
	if (!m_pRT->IsRenderThread())
		DXGLUnbindDeviceContext(GetDeviceContext().GetRealDeviceContext());
	#else
	if (!m_pRT->IsRenderThread())
		DXGLUnbindDeviceContext(GetDeviceContext().GetRealDeviceContext(), !CV_r_multithreaded);
	#endif
#endif //CRY_RENDERER_OPENGL && !DXGL_FULL_EMULATION

	if (!bShaderCacheGen)
	{
		ExecuteRenderThreadCommand([=] { this->RT_Init(); }, ERenderCommandFlags::FlushAndWait);
	}

	if (!g_shaderGeneralHeap)
		g_shaderGeneralHeap = CryGetIMemoryManager()->CreateGeneralExpandingMemoryHeap(4 * 1024 * 1024, 0, "Shader General");

	m_cEF.mfInit();

#if CRY_PLATFORM_WINDOWS
	// Initialize the set of connected monitors
	OnSystemEvent(ESYSTEM_EVENT_DEVICE_CHANGED, 0, 0);
	m_changedMonitor = false;
	m_bWindowRestored = false;
#endif

#if defined(ENABLE_SIMPLE_GPU_TIMERS)
	if (!IsShaderCacheGenMode())
	{
		m_pPipelineProfiler->Init();
	}
#endif

	m_bInitialized = true;

	//  Cry_memcheck();

	// Success, return the window handle
	return (m_hWnd);
}

//==========================================================================
void CD3D9Renderer::InitAMDAPI()
{
#if USE_AMD_API
	do
	{
		AGSReturnCode status = g_pAGSContext ? AGS_SUCCESS : agsInit(&g_pAGSContext, &g_pAGSContextConfiguration, &g_gpuInfo);
		iLog->Log("AGS: AMD GPU Services API init %s (%d)", status == AGS_SUCCESS ? "ok" : "failed", status);
		m_bVendorLibInitialized = status == AGS_SUCCESS;
		if (!m_bVendorLibInitialized)
			break;

		iLog->Log("AGS: Catalyst Version: %s  Driver Version: %s", g_gpuInfo.radeonSoftwareVersion, g_gpuInfo.driverVersion);

		m_nGPUs = 1;
		int numGPUs = 1;
		status = agsGetCrossfireGPUCount(g_pAGSContext, &numGPUs);

		if (status != AGS_SUCCESS)
		{
			iLog->LogError("AGS: Unable to get crossfire info (%d)", status);
		}
		else
		{
			m_nGPUs = numGPUs;
			iLog->Log("AGS: Multi GPU count = %d", numGPUs);
		}

		m_bDeviceSupports_AMDExt = g_extensionsSupported;
	}
	while (0);
#endif   // USE_AMD_API

#if defined(USE_AMD_EXT)
	do
	{
		PFNAmdDxExtCreate11 AmdDxExtCreate = NULL;
		HMODULE hDLL;
		HRESULT hr = S_OK;
		D3DDevice* device = NULL;
		device = GetDevice().GetRealDevice();

	#if defined _WIN64
		hDLL = GetModuleHandle("atidxx64.dll");
	#else
		hDLL = GetModuleHandle("atidxx32.dll");
	#endif

		g_pDepthBoundsTest = NULL;

		// Find the DLL entry point
		if (hDLL)
		{
			AmdDxExtCreate = reinterpret_cast<PFNAmdDxExtCreate11>(GetProcAddress(hDLL, "AmdDxExtCreate11"));
		}
		if (AmdDxExtCreate == NULL)
		{
			g_bDepthBoundsTest = false;
			break;
		}

		// Create the extension object
		hr = AmdDxExtCreate(device, &g_pExtension);

		// Get the Extension Interfaces
		if (SUCCEEDED(hr))
		{
			g_pDepthBoundsTest = static_cast<IAmdDxExtDepthBounds*>(g_pExtension->GetExtInterface(AmdDxExtDepthBoundsID));
		}

		g_bDepthBoundsTest = g_pDepthBoundsTest != NULL;
	}
	while (0);

#endif // USE_AMD_EXT
}

void CD3D9Renderer::InitNVAPI()
{
#if defined(USE_NV_API)
	NvAPI_Status stat = NvAPI_Initialize();
	iLog->Log("NVAPI: API init %s (%d)", stat ? "failed" : "ok", stat);
	m_bVendorLibInitialized = stat == 0;

	if (!m_bVendorLibInitialized)
		return;

	NvU32 version;
	NvAPI_ShortString branch;
	NvAPI_Status status = NvAPI_SYS_GetDriverAndBranchVersion(&version, branch);
	if (status != NVAPI_OK)
		iLog->LogError("NVAPI: Unable to get driver version (%d)", status);

	// enumerate displays
	for (int i = 0; i < NVAPI_MAX_DISPLAYS; ++i)
	{
		NvDisplayHandle displayHandle;
		status = NvAPI_EnumNvidiaDisplayHandle(i, &displayHandle);
		if (status != NVAPI_OK)
			break;
	}

	m_nGPUs = 1;

	// check SLI state to get number of GPUs available for rendering
	NV_GET_CURRENT_SLI_STATE sliState;
	sliState.version = NV_GET_CURRENT_SLI_STATE_VER;

	#if (CRY_RENDERER_DIRECT3D >= 110) && (CRY_RENDERER_DIRECT3D < 120)
	// TODO: add DX12/Vulkan support
	D3DDevice* device = NULL;
	device = GetDevice().GetRealDevice();
	status = NvAPI_D3D_GetCurrentSLIState(device, &sliState);

	if (status != NVAPI_OK)
	{
		iLog->LogError("NVAPI: Unable to get SLI state (%d)", status);
	}
	else
	{
		m_nGPUs = sliState.numAFRGroups;
		if (m_nGPUs < 2)
		{
			iLog->Log("NVAPI: Single GPU system");
		}
		else
		{
			m_nGPUs = min((int)m_nGPUs, 31);
			iLog->Log("NVAPI: System configured as SLI: %d GPU(s) for rendering", m_nGPUs);
		}
	}
	#endif

	m_bDeviceSupports_NVDBT = 1;
	iLog->Log("NVDBT supported");
#endif // USE_NV_API
}

#if CRY_PLATFORM_DURANGO
bool CD3D9Renderer::CreateDeviceDurango()
{
	auto* pDC = GetBaseDisplayContext();

	HRESULT hr = S_OK;

	ID3D11Device* pD3D11Device = nullptr;
	ID3D11DeviceContext* pD3D11Context = nullptr;
#if (CRY_RENDERER_DIRECT3D >= 120)
	CCryDX12Device* pDX12Device = nullptr;
	ID3D12Device* pD3D12Device = nullptr;
#endif

	typedef HRESULT(WINAPI * FP_D3D11CreateDevice)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, CONST D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

	FP_D3D11CreateDevice pD3D11CD =
#if (CRY_RENDERER_DIRECT3D >= 120)
		(FP_D3D11CreateDevice)DX12CreateDevice;
#else
		(FP_D3D11CreateDevice)D3D11CreateDevice;
#endif

	if (pD3D11CD)
	{
		// On Durango we use an internal resolution of 720p but create a 1080p swap chain and do custom upscaling.
		// This is required since the swap chain scaling does just point filtering and since the engine requires the
		// backbuffer to be RGBA and not BGRA as required by the swap chain on Durango.

#if defined(DX11_ALLOW_D3D_DEBUG_RUNTIME)
		const unsigned int debugRTFlag = gcpRendD3D->CV_r_EnableDebugLayer ? D3D11_CREATE_DEVICE_VALIDATED | D3D11_CREATE_DEVICE_DEBUG : 0;
#else
		const unsigned int debugRTFlag = 0;
#endif

		D3D_DRIVER_TYPE driverType = D3D_DRIVER_TYPE_HARDWARE;
		uint32 creationFlags = debugRTFlag
			| D3D11_CREATE_DEVICE_BGRA_SUPPORT
#ifdef USE_PIX_DURANGO
			| D3D11_CREATE_DEVICE_INSTRUMENTED
#endif
			| D3D11_CREATE_DEVICE_FAST_KICKOFFS; // AprilXDK QFE 4

		D3D_FEATURE_LEVEL arrFeatureLevels[] = { CRY_RENDERER_DIRECT3D_FL };
		D3D_FEATURE_LEVEL* pFeatureLevels(arrFeatureLevels);
		unsigned int uNumFeatureLevels(sizeof(arrFeatureLevels) / sizeof(D3D_FEATURE_LEVEL));

		D3D_FEATURE_LEVEL featureLevel;

		HRESULT hr = pD3D11CD(nullptr, driverType, 0, creationFlags, pFeatureLevels, uNumFeatureLevels, D3D11_SDK_VERSION, &pD3D11Device, &featureLevel, &pD3D11Context);
		if (SUCCEEDED(hr) && pD3D11Device && pD3D11Context)
		{
#if (CRY_RENDERER_DIRECT3D >= 120)
			pD3D12Device = (pDX12Device = reinterpret_cast<CCryDX12Device*>(pD3D11Device))->GetD3D12Device();
#endif
		}
	}

	IDXGIDevice1* pDXGIDevice = nullptr;
	IDXGIAdapterToCall* pDXGIAdapter = nullptr;
	IDXGIFactory2ToCall* pDXGIFactory = nullptr;

#if (CRY_RENDERER_DIRECT3D >= 120)
	hr |= pD3D12Device->QueryInterface(IID_GFX_ARGS(&pDXGIDevice));
	hr |= pDXGIDevice->GetAdapter(&pDXGIAdapter);
	hr |= pDXGIAdapter->GetParent(IID_GFX_ARGS(&pDXGIFactory));

	if (pDXGIFactory != nullptr)
	{
		pDC->CreateSwapChain(pDXGIFactory, pD3D12Device, pDX12Device, pDC->GetDisplayResolution().x, pDC->GetDisplayResolution().y);
	}
#else
	hr |= pD3D11Device->QueryInterface(IID_GFX_ARGS(&pDXGIDevice));
	hr |= pDXGIDevice->GetAdapter(&pDXGIAdapter);
	hr |= pDXGIAdapter->GetParent(IID_GFX_ARGS(&pDXGIFactory));

	if (pDXGIFactory != nullptr)
	{
		pDC->CreateSwapChain(pDXGIFactory, pD3D11Device, pDC->GetDisplayResolution().x, pDC->GetDisplayResolution().y);
	}
#endif

	if (!pDXGIFactory || !pDXGIAdapter || !pD3D11Device || !pD3D11Context)
	{
		CryWarning(VALIDATOR_MODULE_RENDERER, VALIDATOR_ERROR, "XBOX::CreateDevice() failed");
		return false;
	}

	{
#if (CRY_RENDERER_DIRECT3D >= 111)
		// TODOL check documentation if it is ok to ignore the context from create device? (both pointers are the same), some refcounting to look out for?\	
		ID3D11Device1* pDevice1 = static_cast<ID3D11Device1*>(pD3D11Device);
		m_DeviceWrapper.AssignDevice(pDevice1);

		D3DDeviceContext* pDeviceContext1 = nullptr;
		GetDevice().GetImmediateContext1(&pDeviceContext1);
		m_DeviceContextWrapper.AssignDeviceContext(pDeviceContext1);
#else
		m_DeviceWrapper.AssignDevice(pD3D11Device);
		m_DeviceContextWrapper.AssignDeviceContext(pDeviceContext);
#endif
	}

#if defined(DEVICE_SUPPORTS_PERFORMANCE_DEVICE)
	{
		// create performance context	and context
		ID3DXboxPerformanceDevice* pPerformanceDevice = nullptr;
		ID3DXboxPerformanceContext* pPerformanceDeviceContext = nullptr;

		hr = GetDevice().QueryInterface(__uuidof(ID3DXboxPerformanceDevice), (void**)&pPerformanceDevice);
		assert(hr == S_OK);

		hr = GetDeviceContext().QueryInterface(__uuidof(ID3DXboxPerformanceContext), (void**)&pPerformanceDeviceContext);
		assert(hr == S_OK);

		m_PerformanceDeviceWrapper.AssignPerformanceDevice(m_pPerformanceDevice = pPerformanceDevice);
		m_PerformanceDeviceContextWrapper.AssignPerformanceDeviceContext(m_pPerformanceDeviceContext = pPerformanceDeviceContext);
	}

	hr = GetPerformanceDevice().SetDriverHint(XBOX_DRIVER_HINT_CONSTANT_BUFFER_OFFSETS_IN_CONSTANTS, 1);
	assert(hr == S_OK);
#endif

#ifdef USE_PIX_DURANGO
	HRESULT res = GetDeviceContext().QueryInterface(__uuidof(ID3DUserDefinedAnnotation), reinterpret_cast<void**>(&m_pPixPerf));
	assert(SUCCEEDED(res));
#endif

	m_DevMan.Init();

	// Post device creation callbacks
	OnD3D11CreateDevice(GetDevice().GetRealDevice());
	OnD3D11PostCreateDevice(GetDevice().GetRealDevice());

	return true;
}
#elif CRY_PLATFORM_IOS || CRY_PLATFORM_ANDROID
bool CD3D9Renderer::CreateDeviceMobile()
{
	auto* pDC = GetBaseDisplayContext();

	if (!m_devInfo.CreateDevice(m_zbpp, OnD3D11CreateDevice, CreateWindowCallback))
		return false;

	InitBaseDisplayContext();
	OnD3D11PostCreateDevice(m_devInfo.Device());

	ChangeWindowProperties(pDC->GetDisplayResolution().x, pDC->GetDisplayResolution().y);

	pDC->ChangeOutputIfNecessary(true, IsVSynced());

	return true;
}
#elif CRY_PLATFORM_WINDOWS || CRY_PLATFORM_APPLE || CRY_PLATFORM_LINUX
bool CD3D9Renderer::CreateDeviceDesktop()
{
	auto* pDC = GetBaseDisplayContext();

	UnSetRes();

	// DirectX9 and DirectX10 device creating
#if defined(SUPPORT_DEVICE_INFO)
	if (!m_devInfo.CreateDevice(m_zbpp, OnD3D11CreateDevice, CreateWindowCallback))
		return false;

	//query adapter name
	const DXGI_ADAPTER_DESC1& desc = m_devInfo.AdapterDesc();
	m_adapterInfo.name = CryStringUtils::WStrToUTF8(desc.Description).c_str();
	m_adapterInfo.vendorId = desc.VendorId;
	m_adapterInfo.deviceId = desc.DeviceId;
	m_adapterInfo.subSysId = desc.SubSysId;
	m_adapterInfo.revision = desc.Revision;
	m_adapterInfo.dedicatedVideoMemory = desc.DedicatedVideoMemory;
	m_adapterInfo.driverVersion = m_devInfo.DriverVersion();
	m_adapterInfo.szDriverBuildNumber = m_devInfo.DriverBuildNumber();

	InitBaseDisplayContext();
	OnD3D11PostCreateDevice(m_devInfo.Device());
#endif

	ChangeWindowProperties(pDC->GetDisplayResolution().x, pDC->GetDisplayResolution().y);

	pDC->ChangeOutputIfNecessary(IsFullscreen(), IsVSynced());

	return true;
}
#elif CRY_RENDERER_GNM
bool CD3D9Renderer::CreateDeviceGNM()
{
	auto* pDC = GetBaseDisplayContext();

	CGnmDevice::Create();

	pDC->CreateSwapChain(pDC->GetDisplayResolution().x, pDC->GetDisplayResolution().y);

	GetDevice().AssignDevice(gGnmDevice);

	BindContextToThread(CryGetCurrentThreadId());
	m_DeviceContextWrapper.AssignDeviceContext(gGnmDevice);

	OnD3D11CreateDevice(gGnmDevice);
	OnD3D11PostCreateDevice(gGnmDevice);

	return true;
}
#elif CRY_PLATFORM_ORBIS
bool CD3D9Renderer::CreateDeviceOrbis()
{
	auto* pDC = GetBaseDisplayContext();

	DXOrbis::CreateCCryDXOrbisRenderDevice();
	DXOrbis::CreateCCryDXOrbisSwapChain();
	DXOrbis::Device()->RegisterDeviceThread();

	GetDevice().AssignDevice(nullptr);
	GetDeviceContext().AssignDeviceContext(nullptr);

	OnD3D11CreateDevice(GetDevice().GetRealDevice());
	OnD3D11PostCreateDevice(GetDevice().GetRealDevice());

	return true;
}
#endif

bool CD3D9Renderer::CreateDevice()
{
	CRY_PROFILE_FUNCTION(PROFILE_LOADING_ONLY);
	ChangeLog();

	m_pixelAspectRatio = 1.0f;
	m_dwCreateFlags = 0;

	///////////////////////////////////////////////////////////////////
#if CRY_PLATFORM_DURANGO
	if (!CreateDeviceDurango())
		return false;

#elif CRY_PLATFORM_IOS || CRY_PLATFORM_ANDROID
	if (!CreateDeviceMobile())
		return false;

#elif CRY_PLATFORM_WINDOWS || CRY_PLATFORM_APPLE || CRY_PLATFORM_LINUX
	if (!CreateDeviceDesktop())
		return false;

#elif CRY_RENDERER_GNM
	if (!CreateDeviceGNM())
		return false;

#elif CRY_PLATFORM_ORBIS
	if (!CreateDeviceOrbis())
		return false;

#else
	#error UNKNOWN RENDER DEVICE PLATFORM
#endif

	m_DevBufMan.Init();

#if defined(ENABLE_RENDER_AUX_GEOM)
	if (m_pRenderAuxGeomD3D)
	{
		m_pRenderAuxGeomD3D->RestoreDeviceObjects();
	}
#endif

	m_pStereoRenderer->InitDeviceAfterD3D();

	SetActiveContext(m_pBaseDisplayContext, {});

	return true;
}

#if (CRY_RENDERER_DIRECT3D >= 110) && NTDDI_WIN10 && (WDK_NTDDI_VERSION >= NTDDI_WIN10) && defined(SUPPORT_DEVICE_INFO)
	#include "dxgi1_4.h"
#endif

void CD3D9Renderer::GetVideoMemoryUsageStats(size_t& vidMemUsedThisFrame, size_t& vidMemUsedRecently, bool bGetPoolsSizes)
{
	if (bGetPoolsSizes)
	{
		vidMemUsedThisFrame = vidMemUsedRecently = (GetTexturesStreamPoolSize() + CV_r_rendertargetpoolsize) * 1024 * 1024;
	}
	else
	{
#if (CRY_RENDERER_DIRECT3D >= 110) && NTDDI_WIN10 && (WDK_NTDDI_VERSION >= NTDDI_WIN10) && defined(SUPPORT_DEVICE_INFO)
		CD3D9Renderer* rd = gcpRendD3D;
		IDXGIAdapter3* pAdapter = nullptr;

	#if (CRY_RENDERER_DIRECT3D >= 120)
		pAdapter = rd->m_devInfo.Adapter();
		pAdapter->AddRef();
	#else
		rd->m_devInfo.Adapter()->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&pAdapter);
	#endif

		DXGI_QUERY_VIDEO_MEMORY_INFO videoMemoryInfoA = {};
		DXGI_QUERY_VIDEO_MEMORY_INFO videoMemoryInfoB = {};

		if (pAdapter)
		{
			pAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &videoMemoryInfoA);
			pAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &videoMemoryInfoB);
			pAdapter->Release();
		}
#else
		struct { SIZE_T CurrentUsage; } videoMemoryInfoA = {};

	#if CRY_RENDERER_GNM
		struct { SIZE_T CurrentUsage; } videoMemoryInfoB = {};
		gGnmDevice->GetMemoryUsageStats(videoMemoryInfoA.CurrentUsage, videoMemoryInfoB.CurrentUsage);
	#endif
#endif

		vidMemUsedThisFrame = size_t(videoMemoryInfoA.CurrentUsage);
		vidMemUsedRecently = 0;
	}
}

//===========================================================================================

#if defined(SUPPORT_DEVICE_INFO) && !CRY_RENDERER_VULKAN
#include <d3d10.h>
#endif

HRESULT CALLBACK CD3D9Renderer::OnD3D11CreateDevice(D3DDevice* pd3dDevice)
{
	CRY_PROFILE_FUNCTION(PROFILE_LOADING_ONLY);
	CD3D9Renderer* rd = gcpRendD3D;
	rd->m_DeviceWrapper.AssignDevice(pd3dDevice);
	GetDeviceObjectFactory().AssignDevice(pd3dDevice);

#if defined(SUPPORT_DEVICE_INFO)
	rd->m_DeviceContextWrapper.AssignDeviceContext(rd->m_devInfo.Context());
#endif
	rd->m_Features |= RFT_OCCLUSIONQUERY | RFT_ALLOWANISOTROPIC | RFT_HW_SM40 | RFT_HW_SM50;

#if defined(DX11_ALLOW_D3D_DEBUG_RUNTIME)
	rd->m_d3dDebug.Init(pd3dDevice);
	rd->m_d3dDebug.Update(ESeverityCombination(CV_d3d11_debugMuteSeverity->GetIVal()), CV_d3d11_debugMuteMsgID->GetString(), CV_d3d11_debugBreakOnMsgID->GetString());
	rd->m_bUpdateD3DDebug = false;
#endif

	GetDeviceObjectFactory().AllocateNullResources();
	GetDeviceObjectFactory().AllocatePredefinedSamplerStates();
	GetDeviceObjectFactory().AllocatePredefinedInputLayouts();

#if defined(SUPPORT_DEVICE_INFO)
	rd->BindContextToThread(CryGetCurrentThreadId());

	LARGE_INTEGER driverVersion;
	driverVersion.LowPart = 0;
	driverVersion.HighPart = 0;
	#if CRY_RENDERER_VULKAN >= 10
	auto version = VK_VERSION_1_0;
	driverVersion.HighPart = (VK_VERSION_MAJOR(version) << 16) | VK_VERSION_MINOR(version);
	driverVersion.LowPart = (VK_VERSION_PATCH(version) << 16) | VK_HEADER_VERSION;
	#else
	// Note  You can use CheckInterfaceSupport only to check whether a Direct3D 10.x interface is
	// supported, and only on Windows Vista SP1 and later versions of the operating system.
	rd->m_devInfo.Adapter()->CheckInterfaceSupport(__uuidof(ID3D10Device), &driverVersion);
	#endif

	iLog->Log("D3D Adapter: Description: %ls", rd->m_devInfo.AdapterDesc().Description);
	iLog->Log("D3D Adapter: Driver version (UMD): %d.%02d.%02d.%04d", HIWORD(driverVersion.u.HighPart), LOWORD(driverVersion.u.HighPart), HIWORD(driverVersion.u.LowPart), LOWORD(driverVersion.u.LowPart));
	iLog->Log("D3D Adapter: VendorId = 0x%.4X", rd->m_devInfo.AdapterDesc().VendorId);
	iLog->Log("D3D Adapter: DeviceId = 0x%.4X", rd->m_devInfo.AdapterDesc().DeviceId);
	iLog->Log("D3D Adapter: SubSysId = 0x%.8X", rd->m_devInfo.AdapterDesc().SubSysId);
	iLog->Log("D3D Adapter: Revision = %i", rd->m_devInfo.AdapterDesc().Revision);

	// Vendor-specific initializations and workarounds for driver bugs.
	{
		if (rd->m_devInfo.AdapterDesc().VendorId == 4098)
		{
			rd->m_Features |= RFT_HW_ATI;
			rd->InitAMDAPI();
			iLog->Log("D3D Detected: AMD video card");
		}
		else if (rd->m_devInfo.AdapterDesc().VendorId == 4318)
		{
			rd->m_Features |= RFT_HW_NVIDIA;
			rd->InitNVAPI();
			iLog->Log("D3D Detected: NVIDIA video card");
		}
		else if (rd->m_devInfo.AdapterDesc().VendorId == 8086)
		{
			rd->m_Features |= RFT_HW_INTEL;
			iLog->Log("D3D Detected: intel video card");
		}
	}

	rd->m_nGPUs = min(rd->m_nGPUs, (uint32)MAX_GPU_NUM);

#endif
	rd->m_adapterInfo.nNodeCount = max(rd->m_nGPUs, rd->m_adapterInfo.nNodeCount);
	CryLogAlways("Active GPUs: %i", rd->m_nGPUs);

#if CRY_PLATFORM_ORBIS
	rd->m_NumResourceSlots = 128;
	rd->m_NumSamplerSlots = 16;
	rd->m_MaxAnisotropyLevel = min(16, CRenderer::CV_r_texmaxanisotropy);
#else
	rd->m_NumResourceSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_REGISTER_COUNT;
	rd->m_NumSamplerSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
	rd->m_MaxAnisotropyLevel = min(D3D11_REQ_MAXANISOTROPY, CRenderer::CV_r_texmaxanisotropy);
#endif

#if CRY_PLATFORM_WINDOWS
	HWND hWndDesktop = GetDesktopWindow();
	HDC dc = GetDC(hWndDesktop);
	uint16 gamma[3][256];
	if (GetDeviceGammaRamp(dc, gamma))
		rd->m_Features |= RFT_HWGAMMA;
	ReleaseDC(hWndDesktop, dc);
#endif

	// For safety, lots of drivers don't handle tiny texture sizes too tell.
#if defined(SUPPORT_DEVICE_INFO)
	rd->m_MaxTextureMemory = rd->m_devInfo.AdapterDesc().DedicatedVideoMemory;
#else
	rd->m_MaxTextureMemory = 256 * 1024 * 1024;

	#if CRY_PLATFORM_DURANGO || CRY_PLATFORM_ORBIS
	rd->m_MaxTextureMemory = 1024 * 1024 * 1024;
	#endif
#endif
	if (CRenderer::CV_r_TexturesStreamPoolSize <= 0)
		CRenderer::CV_r_TexturesStreamPoolSize = (int)(rd->m_MaxTextureMemory / 1024.0f / 1024.0f * 0.75f);

#if CRY_PLATFORM_ORBIS
	rd->m_MaxTextureSize = 8192;
	rd->m_bDeviceSupportsInstancing = true;
#else
	rd->m_MaxTextureSize = D3D11_REQ_FILTERING_HW_ADDRESSABLE_RESOURCE_DIMENSION;
	rd->m_bDeviceSupportsInstancing = true;
#endif

#if !CRY_RENDERER_OPENGLES
	rd->m_bDeviceSupportsGeometryShaders = (rd->m_Features & RFT_HW_SM40) != 0;
#else
	rd->m_bDeviceSupportsGeometryShaders = false;
#endif

#if !CRY_RENDERER_OPENGL && !CRY_RENDERER_VULKAN
	rd->m_bDeviceSupportsTessellation = (rd->m_Features & RFT_HW_SM50) != 0;
#else
	rd->m_bDeviceSupportsTessellation = false;
#endif

	rd->m_Features |= RFT_OCCLUSIONTEST;

	rd->m_bUseWaterTessHW = CV_r_WaterTessellationHW != 0 && rd->m_bDeviceSupportsTessellation;

	PREFAST_SUPPRESS_WARNING(6326); //not a constant vs constant comparison on Win32/Win64
	rd->m_bUseSilhouettePOM = CV_r_SilhouettePOM != 0;

	// Handle the texture formats we need.
	{
		// Find the needed texture formats.
		CRendererResources::s_hwTexFormatSupport.CheckFormatSupport();

		if (CRendererResources::s_hwTexFormatSupport.m_FormatBC1.IsValid() ||
			CRendererResources::s_hwTexFormatSupport.m_FormatBC2.IsValid() ||
			CRendererResources::s_hwTexFormatSupport.m_FormatBC3.IsValid())
		{
			rd->m_Features |= RFT_COMPRESSTEXTURE;
		}

		// One of the two needs to be supported
		if ((rd->m_zbpp == 32) && !CRendererResources::s_hwTexFormatSupport.m_FormatD32FS8.IsValid())
			rd->m_zbpp = 24;
		if ((rd->m_zbpp == 24) && !CRendererResources::s_hwTexFormatSupport.m_FormatD24S8.IsValid())
			rd->m_zbpp = 32;

		iLog->Log(" Renderer DepthBits: %d", rd->m_zbpp);
	}

	rd->m_Features |= RFT_HW_HDR;

	return S_OK;
}

HRESULT CALLBACK CD3D9Renderer::OnD3D11PostCreateDevice(D3DDevice* pd3dDevice)
{
	CRY_PROFILE_FUNCTION(PROFILE_LOADING_ONLY);
	CD3D9Renderer* rd = gcpRendD3D;
	auto* pDC = rd->GetBaseDisplayContext();

	pDC->m_nSSSamplesX = CV_r_Supersampling;
	pDC->m_nSSSamplesY = CV_r_Supersampling;
	pDC->m_bMainViewport = true;

#if DX11_WRAPPABLE_INTERFACE && CAPTURE_REPLAY_LOG
	rd->MemReplayWrapD3DDevice();
#endif

	// Force resize
	const auto displayWidth = pDC->GetDisplayResolution().x;
	const auto displayHeight = pDC->GetDisplayResolution().y;
	pDC->m_DisplayWidth = 0;
	pDC->m_DisplayHeight = 0;
	pDC->ChangeDisplayResolution(displayWidth, displayHeight);

	// Copy swap chain surface desc back to global var
	rd->m_d3dsdBackBuffer = pDC->GetSwapChain().GetSurfaceDesc();

	rd->ReleaseAuxiliaryMeshes();
	rd->CreateAuxiliaryMeshes();

	//rd->ResetToDefault();

	return S_OK;
}

// Change the Window Mode (windowed with border / borderless / fullscreen)
void CD3D9Renderer::UpdateWindowMode()
{
#if CRY_PLATFORM_WINDOWS

	// In non-editor mode
	if (!IsEditorMode())
	{
		// Set self as the active window
		::SetActiveWindow(static_cast<HWND>(GetHWND()));

		// Is the window resizable?
		ICVar* cvResize = gEnv->pConsole->GetCVar("r_resizableWindow");
		bool bFixedSize = ((cvResize != nullptr) && (cvResize->GetIVal() == 0));

		// target style
		DWORD dwStyle = 0;

		EWindowState windowMode = static_cast<EWindowState>(m_CVWindowType->GetIVal());
		switch (windowMode)
		{
		case EWindowState::Windowed:
		{
			// Change to overlapped window style, add a sizing border and controls to resizable windows
			dwStyle = (bFixedSize) ?
				m_dwWinstyleBorder & ~(WS_MAXIMIZEBOX | WS_THICKFRAME) :
				m_dwWinstyleBorder | (WS_MAXIMIZEBOX | WS_THICKFRAME);
		}
		break;
		case EWindowState::BorderlessFullscreen:
			// fallthough intentional
		case EWindowState::BorderlessWindow:
			// fallthough intentional
		case EWindowState::Fullscreen:
			dwStyle = m_dwWinstyleNoBorder;
			break;
		default:
			break;
		}

		// Window handle
		HWND hWnd = static_cast<HWND>(m_hWnd);

		// Client coordinates (to get the uper-left corner)
		RECT targetRect{ 0, 0, 0, 0 };
		::GetClientRect(hWnd, &targetRect);

		// set style, compensate for window borders in windowed mode
		SetWindowLongPtrW(hWnd, GWL_STYLE, dwStyle);

		if (windowMode == EWindowState::Windowed)
		{
			::AdjustWindowRectEx(
				&targetRect
				, dwStyle
				, false
				, static_cast<DWORD>(::GetWindowLongPtr(hWnd, GWL_EXSTYLE))
			);
		}

		// Position the window in the centre of the current monitor
		int nWidth = (targetRect.right - targetRect.left);
		int nHeight = (targetRect.bottom - targetRect.top);
		const RectI bounds = GetBaseDisplayContext()->GetCurrentMonitorBounds();

		// Center window (hide, then move/size, then unhide)
		ShowWindow(hWnd, SW_HIDE);
		SetWindowPos(
			hWnd
			, (windowMode == EWindowState::Fullscreen) ? HWND_TOPMOST : HWND_NOTOPMOST          // Z-Order (fullscreen exclusive = topmost)
			, (windowMode == EWindowState::Fullscreen) ? 0 : ((bounds.w / 2) - (nWidth / 2))    // upper-left X
			, (windowMode == EWindowState::Fullscreen) ? 0 : ((bounds.h / 2) - (nHeight / 2))   // upper-left Y
			, nWidth    // Width
			, nHeight   // Height
			, (SWP_SHOWWINDOW | SWP_DRAWFRAME)    // Flags
		);
		ShowWindow(hWnd, SW_SHOW);

	}

#endif //CRY_PLATFORM_WINDOWS
}

// Change resolution
void CD3D9Renderer::UpdateResolution()
{
#if CRY_PLATFORM_WINDOWS
	// The ISystemEventListener will catch the resize/etc event generated from the ::SetWindowPos call below, and resize graphics resources on that event

	// Set self as the active window
	::SetActiveWindow(static_cast<HWND>(GetHWND()));

	const HWND hWnd = static_cast<HWND>(m_hWnd);
	RECT targetRect{ 0, 0, 0, 0 };

	// Move/size the window to the new coordinates/resolution in non-editor mode
	if (!IsEditorMode())
	{
		// Get client area dimensions in screen space (to use the top-left point)
		::GetClientRect(hWnd, &targetRect);
		::ClientToScreen(hWnd, (POINT*)&targetRect.left);

		// set rectangle width, height (based on existing top-left coords)
		targetRect.right = targetRect.left + m_CVWidth->GetIVal();
		targetRect.bottom = targetRect.top + m_CVHeight->GetIVal();

		const EWindowState windowMode = static_cast<EWindowState>(m_CVWindowType->GetIVal());

		// Make adjustments to the rectangle to compensate for current window decorations
		if (windowMode == EWindowState::Windowed)
		{
			::AdjustWindowRectEx(
				&targetRect
				, static_cast<DWORD>(::GetWindowLongPtr(hWnd, GWL_STYLE))    // window style
				, false    // False = No menu
				, static_cast<DWORD>(::GetWindowLongPtr(hWnd, GWL_EXSTYLE))  // extended style
			);
		}

		// Position the window in the centre of the current monitor
		const int nWidth = (targetRect.right - targetRect.left);
		const int nHeight = (targetRect.bottom - targetRect.top);
		const RectI scrnBounds = GetBaseDisplayContext()->GetCurrentMonitorBounds();
		// Move & size (triggers window events)
		SetWindowPos(
			hWnd
			, (windowMode == EWindowState::Fullscreen) ? HWND_TOPMOST : HWND_NOTOPMOST              // Z-Ordering
			, (windowMode == EWindowState::Fullscreen) ? 0 : ((scrnBounds.w / 2) - (nWidth / 2))    // upper-left X
			, (windowMode == EWindowState::Fullscreen) ? 0 : ((scrnBounds.h / 2) - (nHeight / 2))   // upper-left Y
			, nWidth   // Width
			, nHeight  // Height
			, (SWP_SHOWWINDOW | SWP_FRAMECHANGED)  // Flags
		);

	}
#endif // CRY_PLATFORM_WINDOWS
}

void CD3D9Renderer::OnSystemEvent(ESystemEvent eEvent, UINT_PTR wParam, UINT_PTR lParam)
{

#ifdef CRY_PLATFORM_WINDOWS

	switch (eEvent)
	{
	case ESYSTEM_EVENT_DISPLAY_CHANGED:
	{
		if (!IsEditorMode()) 
		{
			// Resolution has chaned while out of focus - normally another app changing resolution
			if (::GetFocus() != static_cast<HWND>(GetHWND()) && IsFullscreen())
			{
				// Change back to windowed
				m_CVWindowType->Set(static_cast<int>(EWindowState::Windowed));
				// Send event
				gEnv->pSystem->GetISystemEventDispatcher()->OnSystemEvent(ESYSTEM_EVENT_TOGGLE_FULLSCREEN, 0, 0);
			}
		}
	}
	case ESYSTEM_EVENT_RESIZE:
	{
		// Only in editor mode (as the window is externally influenced)
		if (IsEditorMode())
		{
			// Set the width (CVars only trigger when input and stored values differ; this avoids a self-triggering loop)
			m_CVWidth->Set((int)wParam);
			m_CVHeight->Set((int)lParam);
		}
	}
	break;
	default:
		break;
	}
#endif //CRY_PLATFORM_WINDOWS

}

#include "DeviceInfo.inl"

void EnableCloseButton(void* hWnd, bool enabled)
{
#if CRY_PLATFORM_WINDOWS
	if (hWnd)
	{
		HMENU hMenu = GetSystemMenu((HWND) hWnd, FALSE);
		if (hMenu)
		{
			const unsigned int flags = enabled ? MF_ENABLED : (MF_DISABLED | MF_GRAYED);
			EnableMenuItem(hMenu, SC_CLOSE, MF_BYCOMMAND | flags);
		}
	}
#endif
}

#if defined(DX11_ALLOW_D3D_DEBUG_RUNTIME)
string D3DDebug_GetLastMessage()
{
	return gcpRendD3D->m_d3dDebug.GetLastMessage();
}
#endif
