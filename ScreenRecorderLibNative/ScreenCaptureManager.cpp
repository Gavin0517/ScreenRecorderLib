#include "Cleanup.h"
#include <chrono>
#include "ScreenCaptureManager.h"
#include "DesktopDuplicationCapture.h"
#include "WindowsGraphicsCapture.h"
#include "CameraCapture.h"
#include "VideoReader.h"
#include "ImageReader.h"
#include "GifReader.h"
#include <typeinfo>
using namespace DirectX;
using namespace std::chrono;
using namespace std;

DWORD WINAPI CaptureThreadProc(_In_ void *Param);

ScreenCaptureManager::ScreenCaptureManager() :
	m_Device(nullptr),
	m_DeviceContext(nullptr),
	m_TerminateThreadsEvent(nullptr),
	m_LastAcquiredFrameTimeStamp{},
	m_OutputRect{},
	m_SharedSurf(nullptr),
	m_KeyMutex(nullptr),
	m_CaptureThreadCount(0),
	m_CaptureThreadHandles(nullptr),
	m_CaptureThreadData(nullptr),
	m_TextureManager(nullptr),
	m_OverlayManager(nullptr),
	m_IsCapturing(false)
{
	RtlZeroMemory(&m_PtrInfo, sizeof(m_PtrInfo));
	// Event to tell spawned threads to quit
	m_TerminateThreadsEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
}

ScreenCaptureManager::~ScreenCaptureManager()
{
	Clean();
}

//
// Initialize shaders for drawing to screen
//
HRESULT ScreenCaptureManager::Initialize(_In_ ID3D11DeviceContext *pDeviceContext, _In_ ID3D11Device *pDevice)
{
	HRESULT hr = S_OK;
	m_Device = pDevice;
	m_DeviceContext = pDeviceContext;

	m_TextureManager = make_unique<TextureManager>();
	m_OverlayManager = make_unique<OverlayManager>();
	RETURN_ON_BAD_HR(hr = m_TextureManager->Initialize(m_DeviceContext, m_Device));
	RETURN_ON_BAD_HR(hr = m_OverlayManager->Initialize(m_DeviceContext, m_Device));
	return hr;
}

//
// Start up threads for video capture
//
HRESULT ScreenCaptureManager::StartCapture(_In_ std::vector<RECORDING_SOURCE> sources, _In_ std::vector<RECORDING_OVERLAY> overlays, _In_  HANDLE hUnexpectedErrorEvent, _In_  HANDLE hExpectedErrorEvent)
{
	ResetEvent(m_TerminateThreadsEvent);

	HRESULT hr = E_FAIL;
	std::vector<RECORDING_SOURCE_DATA *> CreatedOutputs{};
	RETURN_ON_BAD_HR(hr = CreateSharedSurf(sources, &CreatedOutputs, &m_OutputRect));
	m_CaptureThreadCount = (UINT)(CreatedOutputs.size());
	m_CaptureThreadHandles = new (std::nothrow) HANDLE[m_CaptureThreadCount]{};
	m_CaptureThreadData = new (std::nothrow) CAPTURE_THREAD_DATA[m_CaptureThreadCount]{};
	if (!m_CaptureThreadHandles || !m_CaptureThreadData)
	{
		return E_OUTOFMEMORY;
	}

	HANDLE sharedHandle = GetSharedHandle(m_SharedSurf);
	// Create appropriate # of threads for duplication

	for (UINT i = 0; i < m_CaptureThreadCount; i++)
	{
		RECORDING_SOURCE_DATA *data = CreatedOutputs.at(i);
		m_CaptureThreadData[i].UnexpectedErrorEvent = hUnexpectedErrorEvent;
		m_CaptureThreadData[i].ExpectedErrorEvent = hExpectedErrorEvent;
		m_CaptureThreadData[i].TerminateThreadsEvent = m_TerminateThreadsEvent;
		m_CaptureThreadData[i].CanvasTexSharedHandle = sharedHandle;
		m_CaptureThreadData[i].PtrInfo = &m_PtrInfo;

		m_CaptureThreadData[i].RecordingSource = data;
		RtlZeroMemory(&m_CaptureThreadData[i].RecordingSource->DxRes, sizeof(DX_RESOURCES));
		RETURN_ON_BAD_HR(hr = InitializeDx(nullptr, &m_CaptureThreadData[i].RecordingSource->DxRes));

		DWORD ThreadId;
		m_CaptureThreadHandles[i] = CreateThread(nullptr, 0, CaptureThreadProc, &m_CaptureThreadData[i], 0, &ThreadId);
		if (m_CaptureThreadHandles[i] == nullptr)
		{
			return E_FAIL;
		}
	}
	m_OverlayManager->StartCapture(sharedHandle, overlays, hUnexpectedErrorEvent, hExpectedErrorEvent);
	m_IsCapturing = true;
	return hr;
}

HRESULT ScreenCaptureManager::StopCapture()
{
	if (!SetEvent(m_TerminateThreadsEvent)) {
		LOG_ERROR("Could not terminate capture thread");
		return E_FAIL;
	}
	WaitForThreadTermination();
	m_IsCapturing = false;
	return S_OK;
}

HRESULT ScreenCaptureManager::AcquireNextFrame(_In_  DWORD timeoutMillis, _Inout_ CAPTURED_FRAME *pFrame)
{
	HRESULT hr;
	// Try to acquire keyed mutex in order to access shared surface
	{
		MeasureExecutionTime measure(L"AcquireNextFrame wait for sync");
		hr = m_KeyMutex->AcquireSync(1, timeoutMillis);

	}
	if (hr == static_cast<HRESULT>(WAIT_TIMEOUT))
	{
		return DXGI_ERROR_WAIT_TIMEOUT;
	}
	else if (FAILED(hr))
	{
		return hr;
	}

	ID3D11Texture2D *pDesktopFrame = nullptr;
	{
		ReleaseKeyedMutexOnExit releaseMutex(m_KeyMutex, 0);

		bool haveNewFrameData = (IsUpdatedFramesAvailable() || m_OverlayManager->IsUpdatedFramesAvailable()) && IsInitialFrameWriteComplete();
		if (!haveNewFrameData) {
			return DXGI_ERROR_WAIT_TIMEOUT;
		}
		MeasureExecutionTime measure(L"AcquireNextFrame lock");
		int updatedFrameCount = GetUpdatedFrameCount(true);

		D3D11_TEXTURE2D_DESC desc;
		m_SharedSurf->GetDesc(&desc);
		desc.MiscFlags = 0;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		RETURN_ON_BAD_HR(hr = m_Device->CreateTexture2D(&desc, nullptr, &pDesktopFrame));
		m_DeviceContext->CopyResource(pDesktopFrame, m_SharedSurf);

		int updatedOverlaysCount = 0;
		m_OverlayManager->ProcessOverlays(pDesktopFrame, &updatedOverlaysCount);

		if (updatedFrameCount > 0 || updatedOverlaysCount > 0) {
			QueryPerformanceCounter(&m_LastAcquiredFrameTimeStamp);
		}

		pFrame->Frame = pDesktopFrame;
		pFrame->PtrInfo = &m_PtrInfo;
		pFrame->FrameUpdateCount = updatedFrameCount;
		pFrame->OverlayUpdateCount = updatedOverlaysCount;
	}
	return hr;
}

//
// Clean up resources
//
void ScreenCaptureManager::Clean()
{
	if (m_SharedSurf) {
		m_SharedSurf->Release();
		m_SharedSurf = nullptr;
	}
	if (m_KeyMutex)
	{
		m_KeyMutex->Release();
		m_KeyMutex = nullptr;
	}
	if (m_PtrInfo.PtrShapeBuffer)
	{
		delete[] m_PtrInfo.PtrShapeBuffer;
		m_PtrInfo.PtrShapeBuffer = nullptr;
	}
	RtlZeroMemory(&m_PtrInfo, sizeof(m_PtrInfo));

	if (m_CaptureThreadHandles) {
		for (UINT i = 0; i < m_CaptureThreadCount; ++i)
		{
			if (m_CaptureThreadHandles[i])
			{
				CloseHandle(m_CaptureThreadHandles[i]);
			}
		}
		delete[] m_CaptureThreadHandles;
		m_CaptureThreadHandles = nullptr;
	}

	if (m_CaptureThreadData)
	{
		for (UINT i = 0; i < m_CaptureThreadCount; ++i)
		{
			if (m_CaptureThreadData[i].RecordingSource) {
				CleanDx(&m_CaptureThreadData[i].RecordingSource->DxRes);
				delete m_CaptureThreadData[i].RecordingSource;
				m_CaptureThreadData[i].RecordingSource = nullptr;
			}
		}
		delete[] m_CaptureThreadData;
		m_CaptureThreadData = nullptr;
	}

	m_CaptureThreadCount = 0;

	CloseHandle(m_TerminateThreadsEvent);
}

//
// Waits infinitely for all spawned threads to terminate
//
void ScreenCaptureManager::WaitForThreadTermination()
{
	//if (m_OverlayThreadCount != 0)
	//{
	//	WaitForMultipleObjectsEx(m_OverlayThreadCount, m_OverlayThreadHandles, TRUE, INFINITE, FALSE);
	//}
	if (m_CaptureThreadCount != 0) {
		WaitForMultipleObjectsEx(m_CaptureThreadCount, m_CaptureThreadHandles, TRUE, INFINITE, FALSE);
	}
}

_Ret_maybenull_ CAPTURE_THREAD_DATA *ScreenCaptureManager::GetCaptureDataForRect(RECT rect)
{
	POINT pt{ rect.left,rect.top };
	for (UINT i = 0; i < m_CaptureThreadCount; ++i)
	{
		if (m_CaptureThreadData[i].RecordingSource) {
			if (PtInRect(&m_CaptureThreadData[i].RecordingSource->FrameCoordinates, pt)) {
				return &m_CaptureThreadData[i];
			}
		}
	}
	return nullptr;
}

RECT ScreenCaptureManager::GetSourceRect(_In_ SIZE canvasSize, _In_ RECORDING_SOURCE_DATA *pSource)
{
	int left = pSource->FrameCoordinates.left + pSource->OffsetX;
	int top = pSource->FrameCoordinates.top + pSource->OffsetY;
	return RECT{ left, top, left + RectWidth(pSource->FrameCoordinates),top + RectHeight(pSource->FrameCoordinates) };
}

bool ScreenCaptureManager::IsUpdatedFramesAvailable()
{
	for (UINT i = 0; i < m_CaptureThreadCount; ++i)
	{
		if (m_CaptureThreadData[i].LastUpdateTimeStamp.QuadPart > m_LastAcquiredFrameTimeStamp.QuadPart) {
			return true;
		}
	}
	return false;
}

bool ScreenCaptureManager::IsInitialFrameWriteComplete()
{
	for (UINT i = 0; i < m_CaptureThreadCount; ++i)
	{
		if (m_CaptureThreadData[i].RecordingSource) {
			if (m_CaptureThreadData[i].TotalUpdatedFrameCount == 0) {
				//If any of the recordings have not yet written a frame, we return and wait for them.
				return false;
			}
		}
	}
	return true;
}

UINT ScreenCaptureManager::GetUpdatedFrameCount(_In_ bool resetUpdatedFrameCounts)
{
	int updatedFrameCount = 0;

	for (UINT i = 0; i < m_CaptureThreadCount; ++i)
	{
		if (m_CaptureThreadData[i].LastUpdateTimeStamp.QuadPart > m_LastAcquiredFrameTimeStamp.QuadPart) {
			updatedFrameCount += m_CaptureThreadData[i].UpdatedFrameCountSinceLastWrite;
			if (resetUpdatedFrameCounts) {
				m_CaptureThreadData[i].UpdatedFrameCountSinceLastWrite = 0;
			}
		}
	}
	return updatedFrameCount;
}

HRESULT ScreenCaptureManager::CreateSharedSurf(_In_ std::vector<RECORDING_SOURCE> sources, _Out_ std::vector<RECORDING_SOURCE_DATA *> *pCreatedOutputs, _Out_ RECT *pDeskBounds)
{
	*pCreatedOutputs = std::vector<RECORDING_SOURCE_DATA *>();
	std::vector<std::pair<RECORDING_SOURCE, RECT>> validOutputs;
	HRESULT hr = GetOutputRectsForRecordingSources(sources, &validOutputs);
	if (FAILED(hr)) {
		LOG_ERROR(L"Failed to calculate output rects for recording sources");
		return hr;
	}

	std::vector<RECT> outputRects{};
	for each (auto & pair in validOutputs)
	{
		outputRects.push_back(pair.second);
	}
	std::vector<SIZE> outputOffsets{};
	GetCombinedRects(outputRects, pDeskBounds, &outputOffsets);

	pDeskBounds = &MakeRectEven(*pDeskBounds);
	for (int i = 0; i < validOutputs.size(); i++)
	{
		RECORDING_SOURCE source = validOutputs.at(i).first;
		RECT sourceRect = validOutputs.at(i).second;
		RECORDING_SOURCE_DATA *data = new RECORDING_SOURCE_DATA(source);

		if (source.Type == RecordingSourceType::Display) {
			data->OffsetX -= pDeskBounds->left;
			data->OffsetY -= pDeskBounds->top;

		}
		data->OffsetX -= outputOffsets.at(i).cx;
		data->OffsetY -= outputOffsets.at(i).cy;
		data->FrameCoordinates = sourceRect;
		pCreatedOutputs->push_back(data);
	}

	// Set created outputs
	hr = ScreenCaptureManager::CreateSharedSurf(*pDeskBounds, &m_SharedSurf, &m_KeyMutex);
	return hr;
}

HRESULT ScreenCaptureManager::CreateSharedSurf(_In_ RECT desktopRect, _Outptr_ ID3D11Texture2D **ppSharedTexture, _Outptr_ IDXGIKeyedMutex **ppKeyedMutex)
{
	CComPtr<ID3D11Texture2D> pSharedTexture = nullptr;
	CComPtr<IDXGIKeyedMutex> pKeyedMutex = nullptr;
	// Create shared texture for all capture threads to draw into
	D3D11_TEXTURE2D_DESC DeskTexD;
	RtlZeroMemory(&DeskTexD, sizeof(D3D11_TEXTURE2D_DESC));
	DeskTexD.Width = RectWidth(desktopRect);
	DeskTexD.Height = RectHeight(desktopRect);
	DeskTexD.MipLevels = 1;
	DeskTexD.ArraySize = 1;
	DeskTexD.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	DeskTexD.SampleDesc.Count = 1;
	DeskTexD.Usage = D3D11_USAGE_DEFAULT;
	DeskTexD.BindFlags = D3D11_BIND_RENDER_TARGET;
	DeskTexD.CPUAccessFlags = 0;
	DeskTexD.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

	HRESULT hr = m_Device->CreateTexture2D(&DeskTexD, nullptr, &pSharedTexture);
	if (FAILED(hr))
	{
		LOG_ERROR(L"Failed to create shared texture");
		return hr;
	}
	// Get keyed mutex
	hr = pSharedTexture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(&pKeyedMutex));
	if (FAILED(hr))
	{
		LOG_ERROR(L"Failed to query for keyed mutex in OUTPUTMANAGER");
		return hr;
	}
	if (ppSharedTexture) {
		*ppSharedTexture = pSharedTexture;
		(*ppSharedTexture)->AddRef();
	}
	if (ppKeyedMutex) {
		*ppKeyedMutex = pKeyedMutex;
		(*ppKeyedMutex)->AddRef();
	}
	return hr;
}


DWORD WINAPI CaptureThreadProc(_In_ void *Param)
{
	HRESULT hr = S_OK;
	// D3D objects
	CComPtr<ID3D11Texture2D> SharedSurf = nullptr;
	CComPtr<IDXGIKeyedMutex> KeyMutex = nullptr;

	bool isExpectedError = false;
	bool isUnexpectedError = false;

	// Data passed in from thread creation
	CAPTURE_THREAD_DATA *pData = reinterpret_cast<CAPTURE_THREAD_DATA *>(Param);
	RECORDING_SOURCE_DATA *pSource = pData->RecordingSource;

	//This scope must be here for ReleaseOnExit to work.
	{
		std::unique_ptr<CaptureBase> pRecordingSource = nullptr;
		switch (pSource->Type)
		{
			case RecordingSourceType::CameraCapture: {
				pRecordingSource = make_unique<CameraCapture>();
				break;
			}
			case RecordingSourceType::Display: {
				if (pSource->SourceApi == RecordingSourceApi::DesktopDuplication) {
					pRecordingSource = make_unique<DesktopDuplicationCapture>(pSource->IsCursorCaptureEnabled.value_or(false));
				}
				else if (pSource->SourceApi == RecordingSourceApi::WindowsGraphicsCapture) {
					pRecordingSource = make_unique<WindowsGraphicsCapture>(pSource->IsCursorCaptureEnabled.value_or(false));
				}
				break;
			}
			case RecordingSourceType::Picture: {
				std::string signature = ReadFileSignature(pSource->SourcePath.c_str());
				ImageFileType imageType = getImageTypeByMagic(signature.c_str());
				if (imageType == ImageFileType::IMAGE_FILE_GIF) {
					pRecordingSource = make_unique<GifReader>();
				}
				else {
					pRecordingSource = make_unique<ImageReader>();
				}
				break;
			}
			case RecordingSourceType::Video: {
				pRecordingSource = make_unique<VideoReader>();
				break;
			}
			case RecordingSourceType::Window: {
				pRecordingSource = make_unique<WindowsGraphicsCapture>();
				break;
			}
			default:
				break;
		}

		if (!pRecordingSource) {
			LOG_ERROR(L"Failed to create recording source");
			goto Exit;
		}

		// Obtain handle to sync shared Surface
		hr = pSource->DxRes.Device->OpenSharedResource(pData->CanvasTexSharedHandle, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&SharedSurf));
		if (FAILED(hr))
		{
			LOG_ERROR(L"Opening shared texture failed");
			goto Exit;
		}
		hr = SharedSurf->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(&KeyMutex));
		if (FAILED(hr))
		{
			LOG_ERROR(L"Failed to get keyed mutex interface in spawned thread");
			goto Exit;
		}

		// Make duplication
		hr = pRecordingSource->Initialize(pSource->DxRes.Context, pSource->DxRes.Device);

		if (FAILED(hr))
		{
			LOG_ERROR(L"Failed to initialize recording source");
			goto Exit;
		}
		hr = pRecordingSource->StartCapture(*pSource);

		if (FAILED(hr))
		{
			LOG_ERROR(L"Failed to start capture");
			goto Exit;
		}

		// Main duplication loop
		bool WaitToProcessCurrentFrame = false;
		while (true)
		{
			if (WaitForSingleObjectEx(pData->TerminateThreadsEvent, 0, FALSE) == WAIT_OBJECT_0) {
				hr = S_OK;
				break;
			}

			if (!WaitToProcessCurrentFrame)
			{
				hr = pRecordingSource->AcquireNextFrame(100, nullptr);

				if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
					continue;
				}
				else if (FAILED(hr)) {
					break;
				}
			}
			{
				MeasureExecutionTime measure(L"CaptureThreadProc wait for sync");
				// We have a new frame so try and process it
				// Try to acquire keyed mutex in order to access shared surface
				hr = KeyMutex->AcquireSync(0, 100);
			}
			if (hr == static_cast<HRESULT>(WAIT_TIMEOUT))
			{
				LOG_TRACE(L"CaptureThreadProc shared surface is busy, retrying..");
				// Can't use shared surface right now, try again later
				WaitToProcessCurrentFrame = true;
				continue;
			}
			else if (FAILED(hr))
			{
				// Generic unknown failure
				LOG_ERROR(L"Unexpected error acquiring KeyMutex");
				break;
			}

			MeasureExecutionTime measureLock(L"CaptureThreadProc sync lock");
			ReleaseKeyedMutexOnExit releaseMutex(KeyMutex, 1);

			// We can now process the current frame
			WaitToProcessCurrentFrame = false;

			// Get mouse info
			hr = pRecordingSource->GetMouse(pData->PtrInfo, pSource->IsCursorCaptureEnabled.value_or(false), pSource->FrameCoordinates, pSource->OffsetX, pSource->OffsetY);
			if (FAILED(hr)) {
				LOG_ERROR("Failed to get mouse data");
			}
			hr = pRecordingSource->WriteNextFrameToSharedSurface(0, SharedSurf, pSource->OffsetX, pSource->OffsetY, pSource->FrameCoordinates, pSource->SourceRect);
			if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
				continue;
			}
			else if (FAILED(hr)) {
				break;
			}
			else if (hr == S_FALSE) {
				continue;
			}
			pData->UpdatedFrameCountSinceLastWrite++;
			pData->TotalUpdatedFrameCount++;

			QueryPerformanceCounter(&pData->LastUpdateTimeStamp);
		}
	}
Exit:

	pData->ThreadResult = hr;

	if (FAILED(hr))
	{
		_com_error err(hr);
		switch (hr)
		{
			case DXGI_ERROR_DEVICE_REMOVED:
			case DXGI_ERROR_DEVICE_RESET:
				LOG_ERROR(L"Display device unavailable: %s", err.ErrorMessage());
				isUnexpectedError = true;
				break;
			case E_ACCESSDENIED:
			case DXGI_ERROR_MODE_CHANGE_IN_PROGRESS:
			case DXGI_ERROR_SESSION_DISCONNECTED:
				//case DXGI_ERROR_INVALID_CALL:
			case DXGI_ERROR_ACCESS_LOST:
				//Access to video output is denied, probably due to DRM, screen saver, desktop is switching, fullscreen application is launching, or similar.
				//We continue the recording, and instead of desktop texture just add a blank texture instead.
				isExpectedError = true;
				LOG_WARN(L"Desktop temporarily unavailable: hr = 0x%08x, error = %s", hr, err.ErrorMessage());
				break;
			case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:
				LOG_ERROR(L"Error reinitializing capture with DXGI_ERROR_NOT_CURRENTLY_AVAILABLE. This probably means DXGI reached the limit on the maximum number of concurrent duplication applications (default of four). Therefore, the calling application cannot create any desktop duplication interfaces until the other applications close");
				isUnexpectedError = true;
				break;
			case E_ABORT: {
				//This error is returned when the capture loop should be stopped, but the recording continue.
				break;
			}
			default:
				//Unexpected error, return.
				LOG_ERROR(L"Error reinitializing capture with unexpected error, aborting: %s", err.ErrorMessage());
				isUnexpectedError = true;
				break;
		}
	}

	if (isExpectedError) {
		SetEvent(pData->ExpectedErrorEvent);
	}
	else if (isUnexpectedError) {
		SetEvent(pData->UnexpectedErrorEvent);
	}

	return 0;
}