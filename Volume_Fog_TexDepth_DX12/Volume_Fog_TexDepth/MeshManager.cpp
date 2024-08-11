//======================================================================================
//	Ed Kurlyak 2023 Volume Fog DirectX12
//======================================================================================

#include "MeshManager.h"

CMeshManager::CMeshManager()
{
}

CMeshManager::~CMeshManager()
{
	if (m_d3dDevice != nullptr)
		FlushCommandQueue();
}

void CMeshManager::EnableDebugLayer_CreateFactory()
{
#if defined(DEBUG) || defined(_DEBUG) 
	{
		Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
		ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)));
		debugController->EnableDebugLayer();
	}
#endif

	ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&m_dxgiFactory)));
}

void CMeshManager::Create_Device()
{
	HRESULT hardwareResult = D3D12CreateDevice(
		nullptr,             // default adapter
		D3D_FEATURE_LEVEL_11_0,
		IID_PPV_ARGS(&m_d3dDevice));

	// Fallback to WARP device.
	if (FAILED(hardwareResult))
	{
		Microsoft::WRL::ComPtr<IDXGIAdapter> pWarpAdapter;
		ThrowIfFailed(m_dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&pWarpAdapter)));

		ThrowIfFailed(D3D12CreateDevice(
			pWarpAdapter.Get(),
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&m_d3dDevice)));
	}
}

void CMeshManager::CreateFence_GetDescriptorsSize()
{
	ThrowIfFailed(m_d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE,
		IID_PPV_ARGS(&m_Fence)));

	m_RtvDescriptorSize = m_d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	m_DsvDescriptorSize = m_d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	m_CbvSrvUavDescriptorSize = m_d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void CMeshManager::Check_Multisample_Quality()
{
	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevels;
	msQualityLevels.Format = m_BackBufferFormat;
	msQualityLevels.SampleCount = 4;
	msQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
	msQualityLevels.NumQualityLevels = 0;
	ThrowIfFailed(m_d3dDevice->CheckFeatureSupport(
		D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
		&msQualityLevels,
		sizeof(msQualityLevels)));

	m_4xMsaaQuality = msQualityLevels.NumQualityLevels;
	assert(m_4xMsaaQuality > 0 && "Unexpected MSAA quality level.");
}

void CMeshManager::Create_CommandList_Allocator_Queue()
{
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	ThrowIfFailed(m_d3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_CommandQueue)));

	ThrowIfFailed(m_d3dDevice->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(m_DirectCmdListAlloc.GetAddressOf())));

	ThrowIfFailed(m_d3dDevice->CreateCommandList(
		0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		m_DirectCmdListAlloc.Get(),
		nullptr,
		IID_PPV_ARGS(m_CommandList.GetAddressOf())));

	m_CommandList->Close();
}

void CMeshManager::Create_SwapChain()
{
	m_SwapChain.Reset();

	DXGI_SWAP_CHAIN_DESC sd;
	sd.BufferDesc.Width = m_ClientWidth;
	sd.BufferDesc.Height = m_ClientHeight;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.BufferDesc.Format = m_BackBufferFormat;
	sd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	sd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	sd.SampleDesc.Count = m_4xMsaaState ? 4 : 1;
	sd.SampleDesc.Quality = m_4xMsaaState ? (m_4xMsaaQuality - 1) : 0;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = m_SwapChainBufferCount;
	sd.OutputWindow = m_hWnd;
	sd.Windowed = true;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	ThrowIfFailed(m_dxgiFactory->CreateSwapChain(
		m_CommandQueue.Get(),
		&sd,
		m_SwapChain.GetAddressOf()));
}

void CMeshManager::Resize_SwapChainBuffers()
{
	assert(m_d3dDevice);
	assert(m_SwapChain);
	assert(m_DirectCmdListAlloc);

	FlushCommandQueue();

	ThrowIfFailed(m_CommandList->Reset(m_DirectCmdListAlloc.Get(), nullptr));

	for (int i = 0; i < m_SwapChainBufferCount; ++i)
		m_SwapChainBuffer[i].Reset();

	m_DepthStencilBuffer.Reset();

	ThrowIfFailed(m_SwapChain->ResizeBuffers(
		m_SwapChainBufferCount,
		m_ClientWidth, m_ClientHeight,
		m_BackBufferFormat,
		DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));
}

void CMeshManager::FlushCommandQueue()
{
	m_CurrentFence++;

	ThrowIfFailed(m_CommandQueue->Signal(m_Fence.Get(), m_CurrentFence));

	if (m_Fence->GetCompletedValue() < m_CurrentFence)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);

		ThrowIfFailed(m_Fence->SetEventOnCompletion(m_CurrentFence, eventHandle));

		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}
}

void CMeshManager::Create_Dsv_DescriptorHeaps_And_View_Pass1_Pass2()
{
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 2;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(
		&dsvHeapDesc, IID_PPV_ARGS(m_DsvHeapPass1Pass2.GetAddressOf())));

	m_DSViewHandle_Pass1 = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_DsvHeapPass1Pass2->GetCPUDescriptorHandleForHeapStart());

	m_DSViewHandle_Pass2 = m_DSViewHandle_Pass1;
	m_DSViewHandle_Pass2.Offset(1, m_DsvDescriptorSize);

	D3D12_RESOURCE_DESC depthStencilDesc;
	depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthStencilDesc.Alignment = 0;
	depthStencilDesc.Width = m_ClientWidth;
	depthStencilDesc.Height = m_ClientHeight;
	depthStencilDesc.DepthOrArraySize = 1;
	depthStencilDesc.MipLevels = 1;
	depthStencilDesc.Format = m_DepthStencilFormatPass1_Pass2;
	depthStencilDesc.SampleDesc.Count = m_4xMsaaState ? 4 : 1;
	depthStencilDesc.SampleDesc.Quality = m_4xMsaaState ? (m_4xMsaaQuality - 1) : 0;
	depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE optClear;
	optClear.Format = m_DepthStencilFormatPass1_Pass2;
	optClear.DepthStencil.Depth = 1.0f;
	optClear.DepthStencil.Stencil = 0;
	ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&depthStencilDesc,
		D3D12_RESOURCE_STATE_COMMON,
		&optClear,
		IID_PPV_ARGS(m_DepthTargetTex_Pass1.GetAddressOf())));

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
	dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsvDesc.Format = m_DepthStencilFormatPass1_Pass2;
	dsvDesc.Texture2D.MipSlice = 0;
	m_d3dDevice->CreateDepthStencilView(m_DepthTargetTex_Pass1.Get(), &dsvDesc, m_DSViewHandle_Pass1);

	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_DepthTargetTex_Pass1.Get(),
		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE));

	ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&depthStencilDesc,
		D3D12_RESOURCE_STATE_COMMON,
		&optClear,
		IID_PPV_ARGS(m_DepthTargetTex_Pass2.GetAddressOf())));

	m_d3dDevice->CreateDepthStencilView(m_DepthTargetTex_Pass2.Get(), &dsvDesc, m_DSViewHandle_Pass2);

	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_DepthTargetTex_Pass2.Get(),
		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE));
}

void CMeshManager::Create_Dsv_DescriptorHeaps_And_View_Pass3()
{
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(
		&dsvHeapDesc, IID_PPV_ARGS(m_DsvHeapPass3.GetAddressOf())));

	m_DSViewHandle_Pass3 = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_DsvHeapPass3->GetCPUDescriptorHandleForHeapStart());

	D3D12_RESOURCE_DESC depthStencilDesc;
	depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthStencilDesc.Alignment = 0;
	depthStencilDesc.Width = m_ClientWidth;
	depthStencilDesc.Height = m_ClientHeight;
	depthStencilDesc.DepthOrArraySize = 1;
	depthStencilDesc.MipLevels = 1;
	depthStencilDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
	depthStencilDesc.SampleDesc.Count = m_4xMsaaState ? 4 : 1;
	depthStencilDesc.SampleDesc.Quality = m_4xMsaaState ? (m_4xMsaaQuality - 1) : 0;
	depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE optClear;
	optClear.Format = m_DepthStencilFormatPass3;
	optClear.DepthStencil.Depth = 1.0f;
	optClear.DepthStencil.Stencil = 0;
	ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&depthStencilDesc,
		D3D12_RESOURCE_STATE_COMMON,
		&optClear,
		IID_PPV_ARGS(m_DepthStencilBuffer.GetAddressOf())));

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
	dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsvDesc.Format = m_DepthStencilFormatPass3;
	dsvDesc.Texture2D.MipSlice = 0;
	m_d3dDevice->CreateDepthStencilView(m_DepthStencilBuffer.Get(), &dsvDesc, m_DSViewHandle_Pass3);

	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_DepthStencilBuffer.Get(),
		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE));
}

D3D12_CPU_DESCRIPTOR_HANDLE CMeshManager::DepthStencilView()
{
	return m_DsvHeapPass3->GetCPUDescriptorHandleForHeapStart();
}

void CMeshManager::Execute_Init_Commands()
{
	ThrowIfFailed(m_CommandList->Close());
	ID3D12CommandList* cmdsLists[] = { m_CommandList.Get() };
	m_CommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	FlushCommandQueue();
}

void CMeshManager::Update_ViewPort_And_Scissor()
{
	m_ScreenViewport.TopLeftX = 0;
	m_ScreenViewport.TopLeftY = 0;
	m_ScreenViewport.Width = static_cast<float>(m_ClientWidth);
	m_ScreenViewport.Height = static_cast<float>(m_ClientHeight);
	m_ScreenViewport.MinDepth = 0.0f;
	m_ScreenViewport.MaxDepth = 1.0f;

	m_ScissorRect = { 0, 0, m_ClientWidth, m_ClientHeight };
}

void CMeshManager::Create_Main_RenderTargetHeap_And_View_Pass3()
{
	ThrowIfFailed(m_CommandList->Reset(m_DirectCmdListAlloc.Get(), nullptr));

	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
	rtvHeapDesc.NumDescriptors = m_SwapChainBufferCount;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;
	ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(
		&rtvHeapDesc, IID_PPV_ARGS(m_RtvHeap.GetAddressOf())));

	m_CurrBackBuffer = 0;

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle(m_RtvHeap->GetCPUDescriptorHandleForHeapStart());

	for (UINT i = 0; i < m_SwapChainBufferCount; i++)
	{
		ThrowIfFailed(m_SwapChain->GetBuffer(i, IID_PPV_ARGS(&m_SwapChainBuffer[i])));
		m_d3dDevice->CreateRenderTargetView(m_SwapChainBuffer[i].Get(), nullptr, rtvHeapHandle);
		rtvHeapHandle.Offset(1, m_RtvDescriptorSize);
	}
}

void CMeshManager::Create_RootSignature()
{
	CD3DX12_ROOT_PARAMETER slotRootParameter[3];

	CD3DX12_DESCRIPTOR_RANGE cbvTable0;
	cbvTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);

	CD3DX12_DESCRIPTOR_RANGE cbvTable1;
	cbvTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1);

	CD3DX12_DESCRIPTOR_RANGE srvTable;
	srvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);

	slotRootParameter[0].InitAsDescriptorTable(1, &cbvTable0);
	slotRootParameter[1].InitAsDescriptorTable(1, &cbvTable1);
	slotRootParameter[2].InitAsDescriptorTable(1, &srvTable);

	auto staticSamplers = GetStaticSamplers();

	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, slotRootParameter,
		(UINT)staticSamplers.size(), staticSamplers.data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	Microsoft::WRL::ComPtr<ID3DBlob> SerializedRootSig = nullptr;
	Microsoft::WRL::ComPtr<ID3DBlob> ErrorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		SerializedRootSig.GetAddressOf(), ErrorBlob.GetAddressOf());

	if (ErrorBlob != nullptr)
	{
		::OutputDebugStringA((char*)ErrorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(m_d3dDevice->CreateRootSignature(
		0,
		SerializedRootSig->GetBufferPointer(),
		SerializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(&m_RootSignature)));
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> CMeshManager::GetStaticSamplers()
{
	const CD3DX12_STATIC_SAMPLER_DESC PointWrap(
		0, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC PointClamp(
		1, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC LinearWrap(
		2, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC LinearClamp(
		3, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC AnisotropicWrap(
		4, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressW
		0.0f,                             // mipLODBias
		8);                               // maxAnisotropy

	const CD3DX12_STATIC_SAMPLER_DESC AnisotropicClamp(
		5, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressW
		0.0f,                              // mipLODBias
		8);                                // maxAnisotropy

	return {
		PointWrap, PointClamp,
		LinearWrap, LinearClamp,
		AnisotropicWrap, AnisotropicClamp };
}

void CMeshManager::Create_Cube_Shaders_And_InputLayout_Pass1_Pass2()
{
	m_VsByteCode = d3dUtil::CompileShader(L"Shaders\\depth.hlsl", nullptr, "VS", "vs_5_0");
	m_PsByteCode = d3dUtil::CompileShader(L"Shaders\\depth.hlsl", nullptr, "PS", "ps_5_0");

	m_InputLayout =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
}

void CMeshManager::Create_Cube_Geometry_Pass1_Pass2()
{
	std::array<Vertex, 8> Vertices =
	{
		Vertex({ DirectX::XMFLOAT3(-4.0, -4.0, -4.0) }),	//A
		Vertex({ DirectX::XMFLOAT3(4.0, -4.0, -4.0) }),	//B
		Vertex({ DirectX::XMFLOAT3(-4.0,  4.0, -4.0) }),	//C
		Vertex({ DirectX::XMFLOAT3(4.0,  4.0, -4.0) }),	//D

		Vertex({ DirectX::XMFLOAT3(-4.0, -4.0,  4.0) }),	//E
		Vertex({ DirectX::XMFLOAT3(4.0, -4.0,  4.0) }),	//F
		Vertex({ DirectX::XMFLOAT3(-4.0,  4.0,  4.0) }),	//G
		Vertex({ DirectX::XMFLOAT3(4.0,  4.0,  4.0)  })	//H

	};

	std::array<std::uint16_t, 36> Indices =
	{
		//front face	
		A, C, D,
		A, D, B,

		//back face
		G, E, F,
		G, F, H,

		//left face
		E, G, C,
		E, C, A,

		//right face
		B, D, H,
		B, H, F,

		//top face
		C, G, H,
		C, H, D,

		//bottom face
		E, A, B,
		E, B, F
	};

	const UINT VbByteSize = (UINT)Vertices.size() * sizeof(Vertex);
	const UINT IbByteSize = (UINT)Indices.size() * sizeof(std::uint16_t);

	m_Cube = std::make_unique<MeshGeometry>();
	m_Cube->Name = "Cube";

	m_Cube->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(m_d3dDevice.Get(),
		m_CommandList.Get(), Vertices.data(), VbByteSize, m_Cube->VertexBufferUploader);

	m_Cube->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(m_d3dDevice.Get(),
		m_CommandList.Get(), Indices.data(), IbByteSize, m_Cube->IndexBufferUploader);

	m_Cube->VertexByteStride = sizeof(Vertex);
	m_Cube->VertexBufferByteSize = VbByteSize;
	m_Cube->IndexFormat = DXGI_FORMAT_R16_UINT;
	m_Cube->IndexBufferByteSize = IbByteSize;

	SubmeshGeometry submesh;
	submesh.IndexCount = (UINT)Indices.size();
	submesh.StartIndexLocation = 0;
	submesh.BaseVertexLocation = 0;

	m_Cube->DrawArgs["box"] = submesh;

}

void CMeshManager::Create_Render_Items()
{
	auto boxRitem = std::make_unique<RenderItem>();

	//XMStoreFloat4x4(&boxRitem->World, DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f) * DirectX::XMMatrixTranslation(0.0f, 0.0f, 0.0f));
	boxRitem->World = Identity4x4();
	boxRitem->ObjCBIndex = 0;
	boxRitem->Geo = m_Cube.get();
	boxRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	//boxRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	boxRitem->IndexCount = m_Cube->DrawArgs["box"].IndexCount;
	boxRitem->StartIndexLocation = m_Cube->DrawArgs["box"].StartIndexLocation;
	boxRitem->BaseVertexLocation = m_Cube->DrawArgs["box"].BaseVertexLocation;

	m_AllRitems.push_back(std::move(boxRitem));
}

void CMeshManager::Create_Frame_Resources()
{
	for (int i = 0; i < m_NumFrameResources; ++i)
	{
		m_FrameResources.push_back(std::make_unique<FrameResource>(m_d3dDevice.Get(),
			1, (UINT)m_AllRitems.size()));
	}
}

void CMeshManager::Create_ConstBuff_Descriptors_Heap_And_View()
{
	UINT objCount = (UINT)m_AllRitems.size();

	//object constants � pass constants ������ � ����� ����
	//������� objCount + 1
	UINT numDescriptors = (objCount + 1) * m_NumFrameResources;

	m_PassCbvOffset = objCount * m_NumFrameResources;

	D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc;
	cbvHeapDesc.NumDescriptors = numDescriptors;
	cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	cbvHeapDesc.NodeMask = 0;
	ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&cbvHeapDesc,
		IID_PPV_ARGS(&m_CbvHeap)));

	UINT ObjCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));

	for (int frameIndex = 0; frameIndex < m_NumFrameResources; ++frameIndex)
	{
		auto objectCB = m_FrameResources[frameIndex]->ObjectCB->Resource();
		for (UINT i = 0; i < objCount; ++i)
		{
			D3D12_GPU_VIRTUAL_ADDRESS cbAddress = objectCB->GetGPUVirtualAddress();

			cbAddress += i * ObjCBByteSize;

			int heapIndex = frameIndex * objCount + i;
			auto handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetCPUDescriptorHandleForHeapStart());
			handle.Offset(heapIndex, m_CbvSrvUavDescriptorSize);

			D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
			cbvDesc.BufferLocation = cbAddress;
			cbvDesc.SizeInBytes = ObjCBByteSize;

			m_d3dDevice->CreateConstantBufferView(&cbvDesc, handle);
		}
	}

	UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));

	for (int frameIndex = 0; frameIndex < m_NumFrameResources; ++frameIndex)
	{
		auto passCB = m_FrameResources[frameIndex]->PassCB->Resource();
		D3D12_GPU_VIRTUAL_ADDRESS cbAddress = passCB->GetGPUVirtualAddress();

		int heapIndex = m_PassCbvOffset + frameIndex;
		auto handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetCPUDescriptorHandleForHeapStart());
		handle.Offset(heapIndex, m_CbvSrvUavDescriptorSize);

		D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
		cbvDesc.BufferLocation = cbAddress;
		cbvDesc.SizeInBytes = passCBByteSize;

		m_d3dDevice->CreateConstantBufferView(&cbvDesc, handle);
	}

}

void CMeshManager::Create_PipelineStateObject_Pass1()
{
	//BuildPSO Pass1();
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDescPass1;
	ZeroMemory(&psoDescPass1, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	psoDescPass1.InputLayout = { m_InputLayout.data(), (UINT)m_InputLayout.size() };
	psoDescPass1.pRootSignature = m_RootSignature.Get();
	psoDescPass1.VS =
	{
		reinterpret_cast<BYTE*>(m_VsByteCode->GetBufferPointer()),
		m_VsByteCode->GetBufferSize()
	};
	psoDescPass1.PS =
	{
		reinterpret_cast<BYTE*>(m_PsByteCode->GetBufferPointer()),
		m_PsByteCode->GetBufferSize()
	};
	psoDescPass1.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDescPass1.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDescPass1.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDescPass1.SampleMask = UINT_MAX;
	psoDescPass1.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDescPass1.NumRenderTargets = 1;
	psoDescPass1.RTVFormats[0] = m_BackBufferFormat;
	psoDescPass1.SampleDesc.Count = m_4xMsaaState ? 4 : 1;
	psoDescPass1.SampleDesc.Quality = m_4xMsaaState ? (m_4xMsaaQuality - 1) : 0;
	psoDescPass1.DSVFormat = m_DepthStencilFormatPass1_Pass2;
	ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDescPass1, IID_PPV_ARGS(&m_PSOPass1)));
}

void CMeshManager::Create_PipelineStateObject_Pass2()
{
	CD3DX12_RASTERIZER_DESC desc2 = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);;
	desc2.CullMode = D3D12_CULL_MODE_FRONT;

	//BuildPSO Pass2();
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDescPass2;
	ZeroMemory(&psoDescPass2, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	psoDescPass2.InputLayout = { m_InputLayout.data(), (UINT)m_InputLayout.size() };
	psoDescPass2.pRootSignature = m_RootSignature.Get();
	psoDescPass2.VS =
	{
		reinterpret_cast<BYTE*>(m_VsByteCode->GetBufferPointer()),
		m_VsByteCode->GetBufferSize()
	};
	psoDescPass2.PS =
	{
		reinterpret_cast<BYTE*>(m_PsByteCode->GetBufferPointer()),
		m_PsByteCode->GetBufferSize()
	};
	//psoDescPass2.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDescPass2.RasterizerState = desc2;
	psoDescPass2.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDescPass2.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDescPass2.SampleMask = UINT_MAX;
	psoDescPass2.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDescPass2.NumRenderTargets = 1;
	psoDescPass2.RTVFormats[0] = m_BackBufferFormat;
	psoDescPass2.SampleDesc.Count = m_4xMsaaState ? 4 : 1;
	psoDescPass2.SampleDesc.Quality = m_4xMsaaState ? (m_4xMsaaQuality - 1) : 0;
	psoDescPass2.DSVFormat = m_DepthStencilFormatPass1_Pass2;
	ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDescPass2, IID_PPV_ARGS(&m_PSOPass2)));
}

void CMeshManager::Create_SRDescriptorHead_And_View_For_Pass3()
{
	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc1 = {};
	srvHeapDesc1.NumDescriptors = 2;
	srvHeapDesc1.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc1.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&srvHeapDesc1, IID_PPV_ARGS(&m_SrvDescriptorHeapSAQ)));

	//srv pass1
	CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor1(m_SrvDescriptorHeapSAQ->GetCPUDescriptorHandleForHeapStart());

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc1 = {};
	srvDesc1.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	//srvDesc1.Format = m_DepthTargetTex_Pass1->GetDesc().Format;
	srvDesc1.Format = m_ShaderResourceViewFormatPass3;
	srvDesc1.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc1.Texture2D.MostDetailedMip = 0;
	srvDesc1.Texture2D.MipLevels = m_DepthTargetTex_Pass1->GetDesc().MipLevels;
	srvDesc1.Texture2D.ResourceMinLODClamp = 0.0f;

	m_d3dDevice->CreateShaderResourceView(m_DepthTargetTex_Pass1.Get(), &srvDesc1, hDescriptor1);

	//srv pass2
	hDescriptor1.Offset(1, m_CbvSrvUavDescriptorSize);

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc2 = {};
	srvDesc2.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	//srvDesc2.Format = m_DepthTargetTex_Pass2->GetDesc().Format;
	srvDesc2.Format = m_ShaderResourceViewFormatPass3;
	srvDesc2.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc2.Texture2D.MostDetailedMip = 0;
	srvDesc2.Texture2D.MipLevels = m_DepthTargetTex_Pass2->GetDesc().MipLevels;
	srvDesc2.Texture2D.ResourceMinLODClamp = 0.0f;

	m_d3dDevice->CreateShaderResourceView(m_DepthTargetTex_Pass2.Get(), &srvDesc2, hDescriptor1);
}

void CMeshManager::Create_ScreenAlighedQuad_Shaders_And_InputLayout_Pass3()
{
	m_VsByteCodeSAQ = d3dUtil::CompileShader(L"Shaders\\saq.hlsl", nullptr, "VS", "vs_5_0");
	m_PsByteCodeSAQ = d3dUtil::CompileShader(L"Shaders\\saq.hlsl", nullptr, "PS", "ps_5_0");

	m_InputLayoutSAQ =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
}

void CMeshManager::Create_ScreenAlighedQuad_Geometry_Pass3()
{
	std::array<Vertex, 4> VerticesSAQ =
	{
		Vertex({ DirectX::XMFLOAT3(1.0f,   1.0f, 0.5f) }),
		Vertex({ DirectX::XMFLOAT3(1.0f, -1.0f, 0.5f) }),
		Vertex({ DirectX::XMFLOAT3(-1.0f, 1.0f, 0.5) }),
		Vertex({ DirectX::XMFLOAT3(-1.0f,  -1.0f, 0.5f) })
	};

	const UINT vbSAQByteSize = (UINT)VerticesSAQ.size() * sizeof(Vertex);

	m_SQABuff = std::make_unique<MeshGeometry>();
	m_SQABuff->Name = "SAQ";

	m_SQABuff->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(m_d3dDevice.Get(),
		m_CommandList.Get(), VerticesSAQ.data(), vbSAQByteSize, m_SQABuff->VertexBufferUploader);

	m_SQABuff->VertexByteStride = sizeof(Vertex);
	m_SQABuff->VertexBufferByteSize = vbSAQByteSize;
}

void CMeshManager::Create_PipelineStateObject_Pass3()
{
	//BuildPSO SAQ
	CD3DX12_BLEND_DESC Blend = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	Blend.RenderTarget[0].BlendEnable = true;
	Blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
	Blend.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
	Blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	Blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	Blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
	Blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	Blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDescSAQ;
	ZeroMemory(&psoDescSAQ, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	psoDescSAQ.InputLayout = { m_InputLayoutSAQ.data(), (UINT)m_InputLayoutSAQ.size() };
	psoDescSAQ.pRootSignature = m_RootSignature.Get();
	psoDescSAQ.VS =
	{
		reinterpret_cast<BYTE*>(m_VsByteCodeSAQ->GetBufferPointer()),
		m_VsByteCodeSAQ->GetBufferSize()
	};
	psoDescSAQ.PS =
	{
		reinterpret_cast<BYTE*>(m_PsByteCodeSAQ->GetBufferPointer()),
		m_PsByteCodeSAQ->GetBufferSize()
	};
	psoDescSAQ.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	//psoDescSAQ.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDescSAQ.BlendState = Blend;
	psoDescSAQ.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDescSAQ.SampleMask = UINT_MAX;
	psoDescSAQ.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDescSAQ.NumRenderTargets = 1;
	psoDescSAQ.RTVFormats[0] = m_BackBufferFormat;
	psoDescSAQ.SampleDesc.Count = m_4xMsaaState ? 4 : 1;
	psoDescSAQ.SampleDesc.Quality = m_4xMsaaState ? (m_4xMsaaQuality - 1) : 0;
	psoDescSAQ.DSVFormat = m_DepthStencilFormatPass3;
	ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDescSAQ, IID_PPV_ARGS(&m_PSOSAQ)));
}

D3D12_CPU_DESCRIPTOR_HANDLE CMeshManager::CurrentBackBufferView()
{
	return CD3DX12_CPU_DESCRIPTOR_HANDLE(
		m_RtvHeap->GetCPUDescriptorHandleForHeapStart(),
		m_CurrBackBuffer,
		m_RtvDescriptorSize);
}

ID3D12Resource* CMeshManager::CurrentBackBuffer()
{
	return m_SwapChainBuffer[m_CurrBackBuffer].Get();
}

void CMeshManager::Init_MeshManager(HWND hWnd)
{
	m_hWnd = hWnd;

	EnableDebugLayer_CreateFactory();

	Create_Device();

	CreateFence_GetDescriptorsSize();

	Check_Multisample_Quality();

	Create_CommandList_Allocator_Queue();

	Create_SwapChain();

	Resize_SwapChainBuffers();

	Create_Dsv_DescriptorHeaps_And_View_Pass1_Pass2();

	Create_Dsv_DescriptorHeaps_And_View_Pass3();

	Execute_Init_Commands();

	Update_ViewPort_And_Scissor();

	Create_Main_RenderTargetHeap_And_View_Pass3();

	Create_RootSignature();

	Create_Cube_Shaders_And_InputLayout_Pass1_Pass2();

	Create_Cube_Geometry_Pass1_Pass2();

	Create_Render_Items();

	Create_Frame_Resources();

	Create_ConstBuff_Descriptors_Heap_And_View();

	Create_PipelineStateObject_Pass1();

	Create_PipelineStateObject_Pass2();
	
	Create_SRDescriptorHead_And_View_For_Pass3();

	Create_ScreenAlighedQuad_Shaders_And_InputLayout_Pass3();

	Create_ScreenAlighedQuad_Geometry_Pass3();

	Create_PipelineStateObject_Pass3();

	Execute_Init_Commands();

	DirectX::XMVECTOR Pos = DirectX::XMVectorSet(0, 0.0f, -25.0f, 1.0f);
	DirectX::XMVECTOR Target = DirectX::XMVectorZero();
	DirectX::XMVECTOR Up = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	DirectX::XMMATRIX MatView = DirectX::XMMatrixLookAtLH(Pos, Target, Up);
	DirectX::XMStoreFloat4x4(&m_View, MatView);

	DirectX::XMMATRIX MatProj = DirectX::XMMatrixPerspectiveFovLH(0.25f * DirectX::XM_PI, 4.0f / 3.0f, 1.0f, m_ZFar);
	XMStoreFloat4x4(&m_Proj, MatProj);

	m_Timer.Timer_Start(30);
}

void CMeshManager::Update_MeshManager()
{
	m_Timer.Calculate_FPS();
	float ElapsedTime = m_Timer.Get_Elapsed_Time();

	static float Angle = 0.0f;

	DirectX::XMMATRIX RotY = DirectX::XMMatrixRotationY(Angle);
	DirectX::XMFLOAT4X4 MatWorldY;
	DirectX::XMStoreFloat4x4(&MatWorldY, RotY);

	DirectX::XMMATRIX RotX = DirectX::XMMatrixRotationX(Angle);
	DirectX::XMFLOAT4X4 MatWorldX;
	DirectX::XMStoreFloat4x4(&MatWorldX, RotX);

	Angle += ElapsedTime / 5.0f;
	if (Angle > DirectX::XM_PI * 2.0f)
		Angle = 0.0f;

	DirectX::XMMATRIX WorldY = XMLoadFloat4x4(&MatWorldY);
	DirectX::XMMATRIX WorldX = XMLoadFloat4x4(&MatWorldX);
	DirectX::XMMATRIX World = WorldX * WorldY;
	DirectX::XMMATRIX Proj = XMLoadFloat4x4(&m_Proj);
	DirectX::XMMATRIX MatView = XMLoadFloat4x4(&m_View);

	DirectX::XMMATRIX ViewProj = MatView * Proj;

	m_CurrFrameResourceIndex = (m_CurrFrameResourceIndex + 1) % m_NumFrameResources;
	m_CurrFrameResource = m_FrameResources[m_CurrFrameResourceIndex].get();

	if (m_CurrFrameResource->Fence != 0 && m_Fence->GetCompletedValue() < m_CurrFrameResource->Fence)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
		ThrowIfFailed(m_Fence->SetEventOnCompletion(m_CurrFrameResource->Fence, eventHandle));
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}

	auto currObjectCB = m_CurrFrameResource->ObjectCB.get();

	for (auto& e : m_AllRitems)
	{
		//if (e->NumFramesDirty > 0)
		{
			//DirectX::XMMATRIX World = XMLoadFloat4x4(&e->World);

			ObjectConstants ObjConstants;
			DirectX::XMStoreFloat4x4(&ObjConstants.World, DirectX::XMMatrixTranspose(World));

			currObjectCB->CopyData(e->ObjCBIndex, ObjConstants);

			//e->NumFramesDirty--;
		}
	}

	auto currPassCB = m_CurrFrameResource->PassCB.get();

	PassConstants ObjConstants;
	DirectX::XMStoreFloat4x4(&ObjConstants.ViewProj, DirectX::XMMatrixTranspose(ViewProj));
	ObjConstants.ZFar = m_ZFar;

	currPassCB->CopyData(0, ObjConstants);
}

void CMeshManager::DrawRenderItems_�ube(ID3D12GraphicsCommandList* CmdList, const std::vector<std::unique_ptr<RenderItem>>& Ritems)
{

	UINT ObjCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));

	//auto ObjectCB = m_CurrFrameResource->ObjectCB->Resource();

	for (size_t i = 0; i < Ritems.size(); ++i)
	{
		auto ri = Ritems[i].get();

		CmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
		CmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
		CmdList->IASetPrimitiveTopology(ri->PrimitiveType);

		ID3D12DescriptorHeap* DescriptorHeapsCbv[] = { m_CbvHeap.Get() };
		m_CommandList->SetDescriptorHeaps(_countof(DescriptorHeapsCbv), DescriptorHeapsCbv);

		UINT cbvIndex = m_CurrFrameResourceIndex * (UINT)m_AllRitems.size() + ri->ObjCBIndex;
		auto cbvHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetGPUDescriptorHandleForHeapStart());
		cbvHandle.Offset(cbvIndex, m_CbvSrvUavDescriptorSize);

		CmdList->SetGraphicsRootDescriptorTable(0, cbvHandle);

		CmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);

	}
}

void CMeshManager::Draw_MeshManager()
{
	auto CmdListAlloc = m_CurrFrameResource->CmdListAlloc;

	ThrowIfFailed(CmdListAlloc->Reset());

	ThrowIfFailed(m_CommandList->Reset(CmdListAlloc.Get(), m_PSOPass1.Get()));

	m_CommandList->RSSetViewports(1, &m_ScreenViewport);
	m_CommandList->RSSetScissorRects(1, &m_ScissorRect);

	//pass1
	//------------------------------

	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

	const FLOAT ClearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	m_CommandList->ClearRenderTargetView(CurrentBackBufferView(), ClearColor, 0, nullptr);
	m_CommandList->ClearDepthStencilView(m_DSViewHandle_Pass1, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	m_CommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &m_DSViewHandle_Pass1);

	m_CommandList->SetGraphicsRootSignature(m_RootSignature.Get());

	ID3D12DescriptorHeap* DescriptorHeapsCbv[] = { m_CbvHeap.Get() };
	m_CommandList->SetDescriptorHeaps(_countof(DescriptorHeapsCbv), DescriptorHeapsCbv);

	int PassCbvIndex = m_PassCbvOffset + m_CurrFrameResourceIndex;
	auto passCbvHandle1 = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetGPUDescriptorHandleForHeapStart());
	passCbvHandle1.Offset(PassCbvIndex, m_CbvSrvUavDescriptorSize);
	m_CommandList->SetGraphicsRootDescriptorTable(1, passCbvHandle1);

	DrawRenderItems_�ube(m_CommandList.Get(), m_AllRitems);

	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_DepthTargetTex_Pass1.Get(),
		D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	
	//------------------------------------------
	//pass 2

	m_CommandList->SetPipelineState(m_PSOPass2.Get());

	m_CommandList->ClearRenderTargetView(CurrentBackBufferView(), ClearColor, 0, nullptr);
	m_CommandList->ClearDepthStencilView(m_DSViewHandle_Pass2, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	m_CommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &m_DSViewHandle_Pass2);

	m_CommandList->SetGraphicsRootSignature(m_RootSignature.Get());

	//ID3D12DescriptorHeap* DescriptorHeapsCbv[] = { m_CbvHeap.Get() };
	m_CommandList->SetDescriptorHeaps(_countof(DescriptorHeapsCbv), DescriptorHeapsCbv);

	//int PassCbvIndex = m_PassCbvOffset + m_CurrFrameResourceIndex;
	auto passCbvHandle2 = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetGPUDescriptorHandleForHeapStart());
	passCbvHandle2.Offset(PassCbvIndex, m_CbvSrvUavDescriptorSize);
	m_CommandList->SetGraphicsRootDescriptorTable(1, passCbvHandle2);

	DrawRenderItems_�ube(m_CommandList.Get(), m_AllRitems);

	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_DepthTargetTex_Pass2.Get(),
		D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

	//------------------------------------------
	//Render SAQ

	m_CommandList->SetPipelineState(m_PSOSAQ.Get());

	const FLOAT ClearColor1[4] = { 0.0f, 0.125f, 0.3f, 1.0f };
	m_CommandList->ClearRenderTargetView(CurrentBackBufferView(), ClearColor1, 0, nullptr);
	m_CommandList->ClearDepthStencilView(m_DSViewHandle_Pass3, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	m_CommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &m_DSViewHandle_Pass3);

	m_CommandList->SetGraphicsRootSignature(m_RootSignature.Get());

	ID3D12DescriptorHeap* descriptorHeapsSAQ[] = { m_SrvDescriptorHeapSAQ.Get() };
	m_CommandList->SetDescriptorHeaps(_countof(descriptorHeapsSAQ), descriptorHeapsSAQ);

	m_CommandList->SetGraphicsRootDescriptorTable(2, m_SrvDescriptorHeapSAQ->GetGPUDescriptorHandleForHeapStart());

	m_CommandList->IASetVertexBuffers(0, 1, &m_SQABuff->VertexBufferView());
	m_CommandList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	//����� 4 ������� � ������ � 2 ������������
	m_CommandList->DrawInstanced(4, 2, 0, 0);

	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_DepthTargetTex_Pass1.Get(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE));

	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_DepthTargetTex_Pass2.Get(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE));

	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

	ThrowIfFailed(m_CommandList->Close());

	ID3D12CommandList* cmdsLists[] = { m_CommandList.Get() };
	m_CommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	ThrowIfFailed(m_SwapChain->Present(0, 0));

	m_CurrBackBuffer = (m_CurrBackBuffer + 1) % m_SwapChainBufferCount;

	FlushCommandQueue();
}


