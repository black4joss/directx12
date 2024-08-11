//======================================================================================
//	Ed Kurlyak 2023 Render To Texture DirectX12
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

void CMeshManager::Create_Dsv_DescriptorHeaps_And_View()
{
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(
		&dsvHeapDesc, IID_PPV_ARGS(m_DsvHeap.GetAddressOf())));

	D3D12_RESOURCE_DESC depthStencilDesc;
	depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthStencilDesc.Alignment = 0;
	depthStencilDesc.Width = m_ClientWidth;
	depthStencilDesc.Height = m_ClientHeight;
	depthStencilDesc.DepthOrArraySize = 1;
	depthStencilDesc.MipLevels = 1;
	depthStencilDesc.Format = m_DepthStencilTextureFormat;
	depthStencilDesc.SampleDesc.Count = m_4xMsaaState ? 4 : 1;
	depthStencilDesc.SampleDesc.Quality = m_4xMsaaState ? (m_4xMsaaQuality - 1) : 0;
	depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE optClear;
	optClear.Format = m_DepthStencilFormat;
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
	dsvDesc.Format = m_DepthStencilFormat;
	dsvDesc.Texture2D.MipSlice = 0;
	m_d3dDevice->CreateDepthStencilView(m_DepthStencilBuffer.Get(), &dsvDesc, DepthStencilView());

	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_DepthStencilBuffer.Get(),
		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE));
}

D3D12_CPU_DESCRIPTOR_HANDLE CMeshManager::DepthStencilView()
{
	return m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
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

void CMeshManager::Create_RenderTargetHeap_And_View_For_Pass1()
{
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc1;
	rtvHeapDesc1.NumDescriptors = 1;
	rtvHeapDesc1.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc1.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc1.NodeMask = 0;
	ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(
		&rtvHeapDesc1, IID_PPV_ARGS(m_RtvHeapRTTex.GetAddressOf())));

	m_RTVTexHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_RtvHeapRTTex->GetCPUDescriptorHandleForHeapStart());

	D3D12_RESOURCE_DESC textureDesc = {};
	textureDesc.MipLevels = 1;
	textureDesc.Format = m_BackBufferFormat;
	textureDesc.Width = 800;
	textureDesc.Height = 600;
	textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	textureDesc.DepthOrArraySize = 1;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.SampleDesc.Quality = 0;
	textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

	D3D12_CLEAR_VALUE clearValue = { m_BackBufferFormat, {0.0f, 0.125f, 0.3f, 1.0f  } };

	ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES,
		&textureDesc,
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		&clearValue,
		IID_PPV_ARGS(m_RenderTargetTex.GetAddressOf())));

	m_d3dDevice->CreateRenderTargetView(m_RenderTargetTex.Get(), nullptr, m_RTVTexHandle);
}

void CMeshManager::LoadTextures()
{
	std::vector<std::string> Filename = {
	".\\Rooms\\texture0.bmp",
	".\\Rooms\\texture1.bmp",
	".\\Rooms\\texture2.bmp",
	".\\Rooms\\texture3.bmp",
	".\\Rooms\\texture4.bmp",
	".\\Rooms\\texture5.bmp",
	".\\Rooms\\texture6.bmp",
	".\\Rooms\\texture7.bmp",
	".\\Rooms\\texture8.bmp",
	".\\Rooms\\texture9.bmp",
	".\\Rooms\\texture10.bmp",
	".\\Rooms\\texture11.bmp" };
	
	for (UINT j = 0; j < Filename.size(); j++)
	{
		auto SceneTex = std::make_unique<Texture>();
		SceneTex->Name = "SceneMeshTex";
		//SceneTex->Filename = Filename[j].c_str();

		//��������� BMP ���� ��� ������ � �������� ������
		FILE* Fp = NULL;
		fopen_s(&Fp, Filename[j].c_str(), "rb");
		if (Fp == NULL)
			MessageBox(NULL, L"Error Open File", L"INFO", MB_OK);

		//������ ��������� ����� ��������
		BITMAPFILEHEADER Bfh;
		fread(&Bfh, sizeof(BITMAPFILEHEADER), 1, Fp);

		//������ ��������� ����� ��������
		BITMAPINFOHEADER Bih;
		fread(&Bih, sizeof(BITMAPINFOHEADER), 1, Fp);

		//���������� �� ������ BMP ����� �� ������ ������
		fseek(Fp, Bfh.bfOffBits, SEEK_SET);

		//��������� �� ������ ���� ���������� �� BMP �����
		unsigned char* ResTemp;
		ResTemp = new unsigned char[Bih.biHeight * Bih.biWidth * 4];
		//������ �� ����� rgb ������ �����������
		fread(ResTemp, Bih.biHeight * Bih.biWidth * 4, 1, Fp);

		//��������� �������� ��������� ����
		fclose(Fp);

		TextureWidth = Bih.biWidth;
		TextureHeight = Bih.biHeight;

		unsigned char* Res = new unsigned char[TextureWidth * TextureHeight * 4];

		for (UINT h = 0; h < TextureHeight; h++)
		{
			for (UINT w = 0; w < TextureWidth; w++)
			{
				int Index1 = (h * TextureWidth + w) * 4;
				int Index2 = ((TextureHeight - 1 - h) * TextureWidth + w) * 4;

				//������ RGB �������
				//�������������� BMP ����������� �� ���������
				Res[Index2 + 0] = ResTemp[Index1 + 2];
				Res[Index2 + 1] = ResTemp[Index1 + 1];
				Res[Index2 + 2] = ResTemp[Index1 + 0];
				Res[Index2 + 3] = ResTemp[Index1 + 3];

			}
		}

		SceneTex->Resource = CreateTexture(m_d3dDevice.Get(),
			m_CommandList.Get(), Res, TextureWidth * TextureHeight * 4, SceneTex->UploadHeap);

		delete[] Res;

		m_Scene[j]->Textures[SceneTex->Name] = std::move(SceneTex);
	}
}

Microsoft::WRL::ComPtr<ID3D12Resource> CMeshManager::CreateTexture(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* cmdList,
	const void* initData,
	UINT64 byteSize,
	Microsoft::WRL::ComPtr<ID3D12Resource>& UploadBuffer)
{
	Microsoft::WRL::ComPtr<ID3D12Resource> m_Texture;
	
	D3D12_RESOURCE_DESC textureDesc = {};
	textureDesc.MipLevels = 1;
	textureDesc.Format = m_BackBufferFormat;
	textureDesc.Width = TextureWidth;
	textureDesc.Height = TextureHeight;
	textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	textureDesc.DepthOrArraySize = 1;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.SampleDesc.Quality = 0;
	textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&textureDesc,
		//D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(m_Texture.GetAddressOf())));

	const UINT64 UploadBufferSize = GetRequiredIntermediateSize(m_Texture.Get(), 0, 1);

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(UploadBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&UploadBuffer)));

	D3D12_SUBRESOURCE_DATA textureData = {};
	textureData.pData = initData;
	textureData.RowPitch = TextureWidth * 4;
	textureData.SlicePitch = textureData.RowPitch * TextureHeight;

	cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_Texture.Get(),
		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
	
	UpdateSubresources(cmdList, m_Texture.Get(), UploadBuffer.Get(), 0, 0, 1, &textureData);

	cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_Texture.Get(),
		D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

	return m_Texture;
}

void CMeshManager::Create_ShaderResource_Heap_And_View_Pass1()
{
	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.NumDescriptors = MeshNums;
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_SrvDescriptorHeap)));

	for (int j = 0; j < MeshNums; j++)
	{


		CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(m_SrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
		hDescriptor.Offset(j, m_CbvSrvUavDescriptorSize);

		auto SceneMeshTex = m_Scene[j]->Textures["SceneMeshTex"]->Resource;

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = SceneMeshTex->GetDesc().Format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = SceneMeshTex->GetDesc().MipLevels;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

		m_d3dDevice->CreateShaderResourceView(SceneMeshTex.Get(), &srvDesc, hDescriptor);
	}

}

void CMeshManager::Create_Main_RenderTargetHeap_And_View_Pass2()
{
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

void CMeshManager::Create_ShaderRVHeap_And_View_Pass2()
{
	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.NumDescriptors = 1;
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_SrvDescriptorHeapSAQ)));

	CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor1(m_SrvDescriptorHeapSAQ->GetCPUDescriptorHandleForHeapStart());

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = m_RenderTargetTex->GetDesc().Format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = m_RenderTargetTex->GetDesc().MipLevels;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

	m_d3dDevice->CreateShaderResourceView(m_RenderTargetTex.Get(), &srvDesc, hDescriptor1);
}

void CMeshManager::Create_Mesh_Shaders_And_InputLayout_Pass1()
{
	m_VsByteCode = d3dUtil::CompileShader(L"Shaders\\tex.hlsl", nullptr, "VS", "vs_5_0");
	m_PsByteCode = d3dUtil::CompileShader(L"Shaders\\tex.hlsl", nullptr, "PS", "ps_5_0");

	m_InputLayout =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
}

void CMeshManager::Create_Mesh_Geometry_Pass1()
{
	ThrowIfFailed(m_CommandList->Reset(m_DirectCmdListAlloc.Get(), nullptr));

	std::vector<std::string> Filename = {
		".\\Rooms\\room0.txt",
		".\\Rooms\\room1.txt",
		".\\Rooms\\room2.txt",
		".\\Rooms\\room3.txt",
		".\\Rooms\\room4.txt",
		".\\Rooms\\room5.txt",
		".\\Rooms\\room6.txt",
		".\\Rooms\\room7.txt",
		".\\Rooms\\room8.txt",
		".\\Rooms\\room9.txt",
		".\\Rooms\\room10.txt",
		".\\Rooms\\room11.txt" };
	
	for ( UINT j = 0; j < Filename.size(); j++)
	{

		FILE* f;
		fopen_s(&f, Filename[j].c_str(), "rt");

		char Buffer[1024];

		//�������� ��� ����� ��������
		fgets(Buffer, 1024, f);

		//�������� ���������� ������
		fgets(Buffer, 1024, f);

		//����� ���������� ������
		int Size;
		sscanf_s(Buffer, "%d", &Size);

		std::vector<Vertex> Vertices;
		Vertices.resize(Size);

		for (int i = 0; i < Size; i++)
		{
			fgets(Buffer, 1024, f);
			sscanf_s(Buffer, "%f;%f;%f;%f;%f", &Vertices[i].Pos.x,
				&Vertices[i].Pos.y,
				&Vertices[i].Pos.z,
				&Vertices[i].Tex.x,
				&Vertices[i].Tex.y);
		}

		fclose(f);

		const UINT VbByteSize = (UINT)Vertices.size() * sizeof(Vertex);

		m_Scene[j] = std::make_unique<MeshGeometry>();
		m_Scene[j]->Name = "Scene";

		m_Scene[j]->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(m_d3dDevice.Get(),
			m_CommandList.Get(), Vertices.data(), VbByteSize, m_Scene[j]->VertexBufferUploader);

		m_Scene[j]->VertexByteStride = sizeof(Vertex);
		m_Scene[j]->VertexBufferByteSize = VbByteSize;

		SubmeshGeometry submesh;
		submesh.VertexCount = (UINT)Vertices.size();
		submesh.StartIndexLocation = 0;
		submesh.BaseVertexLocation = 0;

		m_Scene[j]->DrawArgs = submesh;
	}
}

void CMeshManager::Create_Render_Items()
{
	for ( int i = 0; i < MeshNums; i++ )
	{
		auto boxRitem = std::make_unique<RenderItem>();

		//XMStoreFloat4x4(&boxRitem->World, DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f) * DirectX::XMMatrixTranslation(0.0f, 0.0f, 0.0f));
		boxRitem->World = Identity4x4();
		boxRitem->ObjCBIndex = 0;
		boxRitem->Geo = m_Scene[i].get();
		boxRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		//boxRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		boxRitem->VertexCount = m_Scene[i]->DrawArgs.VertexCount;
		boxRitem->StartIndexLocation = m_Scene[i]->DrawArgs.StartIndexLocation;
		boxRitem->BaseVertexLocation = m_Scene[i]->DrawArgs.BaseVertexLocation;

		m_AllRitems.push_back(std::move(boxRitem));

	}
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


void CMeshManager::Create_RootSignature()
{
	CD3DX12_DESCRIPTOR_RANGE cbvTable0;
	cbvTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);

	CD3DX12_DESCRIPTOR_RANGE cbvTable1;
	cbvTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1);

	CD3DX12_DESCRIPTOR_RANGE srvTable;
	srvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

	CD3DX12_ROOT_PARAMETER slotRootParameter[3];

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

void CMeshManager::Create_PipelineStateObject_Pass1()
{
	CD3DX12_RASTERIZER_DESC desc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);;
	//desc.CullMode = D3D12_CULL_MODE_FRONT;
	desc.CullMode = D3D12_CULL_MODE_BACK;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
	ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	psoDesc.InputLayout = { m_InputLayout.data(), (UINT)m_InputLayout.size() };
	psoDesc.pRootSignature = m_RootSignature.Get();
	psoDesc.VS =
	{
		reinterpret_cast<BYTE*>(m_VsByteCode->GetBufferPointer()),
		m_VsByteCode->GetBufferSize()
	};
	psoDesc.PS =
	{
		reinterpret_cast<BYTE*>(m_PsByteCode->GetBufferPointer()),
		m_PsByteCode->GetBufferSize()
	};
	//psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState = desc; 
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = m_BackBufferFormat;
	psoDesc.SampleDesc.Count = m_4xMsaaState ? 4 : 1;
	psoDesc.SampleDesc.Quality = m_4xMsaaState ? (m_4xMsaaQuality - 1) : 0;
	psoDesc.DSVFormat = m_DepthStencilFormat;
	ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_PSO)));
}

void CMeshManager::Create_ScreenAlignedQuad_Shaders_And_InputLayout_Pass2()
{
	m_VsByteCodeSAQ = d3dUtil::CompileShader(L"Shaders\\saq.hlsl", nullptr, "VS", "vs_5_0");
	m_PsByteCodeSAQ = d3dUtil::CompileShader(L"Shaders\\saq.hlsl", nullptr, "PS", "ps_5_0");

	m_InputLayoutSAQ =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
}

void CMeshManager::Create_ScreenAlignedQuad_Geometry_Pass2()
{
	std::array<VertexSAQ, 4> VerticesSAQ =
	{
		VertexSAQ({ DirectX::XMFLOAT3(1.0f,   1.0f, 0.5f) }),
		VertexSAQ({ DirectX::XMFLOAT3(1.0f, -1.0f, 0.5f) }),
		VertexSAQ({ DirectX::XMFLOAT3(-1.0f, 1.0f, 0.5) }),
		VertexSAQ({ DirectX::XMFLOAT3(-1.0f,  -1.0f, 0.5f) })
	};

	const UINT vbSAQByteSize = (UINT)VerticesSAQ.size() * sizeof(VertexSAQ);

	m_SQABuff = std::make_unique<MeshGeometry>();
	m_SQABuff->Name = "SAQ";
	
	m_SQABuff->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(m_d3dDevice.Get(),
		m_CommandList.Get(), VerticesSAQ.data(), vbSAQByteSize, m_SQABuff->VertexBufferUploader);

	m_SQABuff->VertexByteStride = sizeof(VertexSAQ);
	m_SQABuff->VertexBufferByteSize = vbSAQByteSize;
}

void CMeshManager::Create_PipelineStateObject_Pass2()
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc_SAQ;
	ZeroMemory(&psoDesc_SAQ, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	psoDesc_SAQ.InputLayout = { m_InputLayoutSAQ.data(), (UINT)m_InputLayoutSAQ.size() };
	psoDesc_SAQ.pRootSignature = m_RootSignature.Get();
	psoDesc_SAQ.VS =
	{
		reinterpret_cast<BYTE*>(m_VsByteCodeSAQ->GetBufferPointer()),
		m_VsByteCodeSAQ->GetBufferSize()
	};
	psoDesc_SAQ.PS =
	{
		reinterpret_cast<BYTE*>(m_PsByteCodeSAQ->GetBufferPointer()),
		m_PsByteCodeSAQ->GetBufferSize()
	};
	psoDesc_SAQ.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc_SAQ.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc_SAQ.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc_SAQ.SampleMask = UINT_MAX;
	psoDesc_SAQ.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc_SAQ.NumRenderTargets = 1;
	psoDesc_SAQ.RTVFormats[0] = m_BackBufferFormat;
	psoDesc_SAQ.SampleDesc.Count = m_4xMsaaState ? 4 : 1;
	psoDesc_SAQ.SampleDesc.Quality = m_4xMsaaState ? (m_4xMsaaQuality - 1) : 0;
	psoDesc_SAQ.DSVFormat = m_DepthStencilFormat;
	ThrowIfFailed(m_d3dDevice->CreateGraphicsPipelineState(&psoDesc_SAQ, IID_PPV_ARGS(&m_PSOSAQ)));
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

	m_Camera.Init_Camera(m_ClientWidth, m_ClientHeight);

	EnableDebugLayer_CreateFactory();

	Create_Device();

	CreateFence_GetDescriptorsSize();

	Check_Multisample_Quality();

	Create_CommandList_Allocator_Queue();

	Create_SwapChain();

	Resize_SwapChainBuffers();

	Create_Dsv_DescriptorHeaps_And_View();

	Execute_Init_Commands();

	Update_ViewPort_And_Scissor();

	Create_RenderTargetHeap_And_View_For_Pass1();

	Create_Main_RenderTargetHeap_And_View_Pass2();

	Create_ShaderRVHeap_And_View_Pass2();

	Create_Mesh_Shaders_And_InputLayout_Pass1();

	Create_Mesh_Geometry_Pass1();

	LoadTextures();

	Create_ShaderResource_Heap_And_View_Pass1();

	Create_Render_Items();

	Create_Frame_Resources();

	Create_ConstBuff_Descriptors_Heap_And_View();

	Create_RootSignature();

	Create_PipelineStateObject_Pass1();

	Create_ScreenAlignedQuad_Shaders_And_InputLayout_Pass2();

	Create_ScreenAlignedQuad_Geometry_Pass2();

	Create_PipelineStateObject_Pass2();

	Execute_Init_Commands();

	DirectX::XMVECTOR Pos = DirectX::XMVectorSet(25.0f, 5.0f, -5000.0f, 1.0f);
	//DirectX::XMVECTOR Target = DirectX::XMVectorZero();
	DirectX::XMVECTOR Target = DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
	DirectX::XMVECTOR Up = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	DirectX::XMMATRIX MatView = DirectX::XMMatrixLookAtLH(Pos, Target, Up);
	DirectX::XMStoreFloat4x4(&m_View, MatView);

	DirectX::XMMATRIX MatProj = DirectX::XMMatrixPerspectiveFovLH(0.25f * DirectX::XM_PI, 4.0f / 3.0f, 1.0f, 50000.0f);
	XMStoreFloat4x4(&m_Proj, MatProj);
	
	m_Timer.Timer_Start(30);
}

void CMeshManager::Update_MeshManager()
{
	m_Timer.Calculate_FPS();
	float ElapsedTime = m_Timer.Get_Elapsed_Time();

	DirectX::XMMATRIX World = XMLoadFloat4x4(&m_World);
	DirectX::XMMATRIX Proj = XMLoadFloat4x4(&m_Proj);
	DirectX::XMMATRIX MatView = m_Camera.Frame_Move(ElapsedTime);

	//��� ������� �������� �� �������
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
	DirectX::XMStoreFloat3(&ObjConstants.VecCamPos, m_Camera.VecCamPos);
	currPassCB->CopyData(0, ObjConstants);
}

D3D12_CPU_DESCRIPTOR_HANDLE CMeshManager::GetCurrentSrvView(int Num)
{
	return CD3DX12_CPU_DESCRIPTOR_HANDLE(
		m_SrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
		Num,
		m_CbvSrvUavDescriptorSize);
}

void CMeshManager::DrawRenderItems_Scene(ID3D12GraphicsCommandList* CmdList, const std::vector<std::unique_ptr<RenderItem>>& Ritems)
{

	//CmdList->SetGraphicsRootDescriptorTable(2, m_SrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

	UINT ObjCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));

	//auto ObjectCB = m_CurrFrameResource->ObjectCB->Resource();

	for (size_t i = 0; i < Ritems.size(); ++i)
	{
		auto ri = Ritems[i].get();

		CmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
		CmdList->IASetPrimitiveTopology(ri->PrimitiveType);

		UINT cbvIndex = m_CurrFrameResourceIndex * (UINT)m_AllRitems.size() + ri->ObjCBIndex;
		auto cbvHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetGPUDescriptorHandleForHeapStart());
		cbvHandle.Offset(cbvIndex, m_CbvSrvUavDescriptorSize);

		ID3D12DescriptorHeap* DescriptorHeapsCbv[] = { m_CbvHeap.Get() };
		m_CommandList->SetDescriptorHeaps(_countof(DescriptorHeapsCbv), DescriptorHeapsCbv);

		CmdList->SetGraphicsRootDescriptorTable(0, cbvHandle);

		ID3D12DescriptorHeap* descriptorHeapsSRV[] = { m_SrvDescriptorHeap.Get() };
		m_CommandList->SetDescriptorHeaps(_countof(descriptorHeapsSRV), descriptorHeapsSRV);

		CD3DX12_GPU_DESCRIPTOR_HANDLE hDescriptor(m_SrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		hDescriptor.Offset(i, m_CbvSrvUavDescriptorSize);

		CmdList->SetGraphicsRootDescriptorTable(2, hDescriptor);

		CmdList->DrawInstanced(
			ri->VertexCount, 1, 0, 0);
	}
}

void CMeshManager::Draw_MeshManager()
{
	auto CmdListAlloc = m_CurrFrameResource->CmdListAlloc;

	ThrowIfFailed(CmdListAlloc->Reset());

	ThrowIfFailed(m_CommandList->Reset(CmdListAlloc.Get(), m_PSO.Get()));

	m_CommandList->RSSetViewports(1, &m_ScreenViewport);
	m_CommandList->RSSetScissorRects(1, &m_ScissorRect);

	//PASS1
	//Draw scene to my RTV texture
	//------------------------------

	float ClearColor[4] = { 0.0f, 0.125f, 0.3f, 1.0f };
	m_CommandList->ClearRenderTargetView(m_RTVTexHandle, ClearColor, 0, nullptr);
	m_CommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	m_CommandList->OMSetRenderTargets(1, &m_RTVTexHandle, true, &DepthStencilView());

	m_CommandList->SetGraphicsRootSignature(m_RootSignature.Get());

	
	ID3D12DescriptorHeap* DescriptorHeapsCbv[] = { m_CbvHeap.Get() };
	m_CommandList->SetDescriptorHeaps(_countof(DescriptorHeapsCbv), DescriptorHeapsCbv);

	int PassCbvIndex = m_PassCbvOffset + m_CurrFrameResourceIndex;
	auto passCbvHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetGPUDescriptorHandleForHeapStart());
	passCbvHandle.Offset(PassCbvIndex, m_CbvSrvUavDescriptorSize);
	m_CommandList->SetGraphicsRootDescriptorTable(1, passCbvHandle);

	DrawRenderItems_Scene(m_CommandList.Get(), m_AllRitems);

	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_RenderTargetTex.Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

	//PASS2
	//Render Screen Alighed Quad
	//------------------------------------------

	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

	m_CommandList->SetPipelineState(m_PSOSAQ.Get());

	float ClearColor1[4] = { 0.0f, 0.125f, 0.3f, 1.0f };
	m_CommandList->ClearRenderTargetView(CurrentBackBufferView(), ClearColor1, 0, nullptr);
	m_CommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	m_CommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

	m_CommandList->SetGraphicsRootSignature(m_RootSignature.Get());

	ID3D12DescriptorHeap* descriptorHeapsSAQ[] = { m_SrvDescriptorHeapSAQ.Get() };
	m_CommandList->SetDescriptorHeaps(_countof(descriptorHeapsSAQ), descriptorHeapsSAQ);

	m_CommandList->SetGraphicsRootDescriptorTable(2, m_SrvDescriptorHeapSAQ->GetGPUDescriptorHandleForHeapStart());

	m_CommandList->IASetVertexBuffers(0, 1, &m_SQABuff->VertexBufferView());
	m_CommandList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	//����� 4 ������� � ������ � 2 ������������
	m_CommandList->DrawInstanced(4, 2, 0, 0);

	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_RenderTargetTex.Get(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));

	m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

	ThrowIfFailed(m_CommandList->Close());

	ID3D12CommandList* cmdsLists[] = { m_CommandList.Get() };
	m_CommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	ThrowIfFailed(m_SwapChain->Present(0, 0));

	m_CurrBackBuffer = (m_CurrBackBuffer + 1) % m_SwapChainBufferCount;

	FlushCommandQueue();
}


