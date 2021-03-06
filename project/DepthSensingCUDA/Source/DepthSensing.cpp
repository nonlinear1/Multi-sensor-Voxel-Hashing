﻿
#include "stdafx.h"
#include "DepthSensing.h"
#include "SensorDataReader.h"
#include "Profiler.h"
#include "MultiSensor.h"

#define ENABLE_PROFILE
#ifdef ENABLE_PROFILE
#define PROFILE_CODE(CODE) CODE
#else
#define PROFILE_CODE(CODE)
#endif
	

//--------------------------------------------------------------------------------------
// Global variables
//--------------------------------------------------------------------------------------

CDXUTDialogResourceManager	g_DialogResourceManager; // manager for shared resources of dialogs
CDXUTTextHelper*            g_pTxtHelper = NULL;
bool						g_renderText = true;
bool						g_bRenderHelp = true;

// 15769 Multi sensors
#define MAX_SENSORS (10)
size_t		g_numSensors = 1;

CModelViewerCamera          g_Camera;               // A model viewing camera
CUDARGBDSensor				g_CudaDepthSensor;
CUDARGBDAdapter				g_RGBDAdapter;
DX11RGBDRenderer			g_RGBDRenderer;
DX11CustomRenderTarget		g_CustomRenderTarget;

CUDACameraTrackingMultiRes*		g_cameraTracking	 = NULL;
CUDACameraTrackingMultiResRGBD*	g_cameraTrackingRGBD = NULL;

CUDASceneRepHashSDF*		g_sceneRep			= NULL;
CUDARayCastSDF*				g_rayCast			= NULL;
CUDAMarchingCubesHashSDF*	g_marchingCubesHashSDF = NULL;
CUDAHistrogramHashSDF*		g_historgram = NULL;
CUDASceneRepChunkGrid*		g_chunkGrid = NULL;

size_t	g_renderSensorId = 0;
bool g_bProcessSensor = true;

#pragma region scheduler

class MultiFrameScheduler{
public:
	struct FrameRequest{
		FrameRequest(const mat4f _transformation,
			const DepthCameraData depthCameraData, 
			const DepthCameraParams depthCameraParams,
			size_t _sensor_id, std::string _tag) :
			transformation(_transformation), 
			depthCameraData(depthCameraData), depthCameraParams(depthCameraParams), 
			sensor_id(_sensor_id), tag(_tag) {}

		mat4f transformation;
		DepthCameraData depthCameraData;
		DepthCameraParams depthCameraParams;
		size_t sensor_id;
		std::string tag;
	};

	void add_request(FrameRequest request){
		requests_.push_back(request);
	}

	int num_request() {
		return requests_.size();
	}

	/**
	 * Ideally the schedule can be agnostic of sensor IDs.
	 * It only sees a bunch of frames to be integrated.
	 */
	virtual void schedule_and_execute(){
#ifdef BINARY_DUMP_READER
		assert(GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_MultiSensor);
		assert(GlobalAppState::get().s_binaryDumpSensorUseTrajectory);
		for (size_t i = 0; i < requests_.size(); i++){
			execute_frame_request(requests_[i]);
		} 
		requests_.erase(requests_.begin(), requests_.begin() + requests_.size() - 1);
#endif
	}

protected:
	std::vector<FrameRequest> requests_;
	size_t num_processed_frames_ = 0;

	void execute_frame_request(const FrameRequest& req){
		std::cout << "Executing: " + req.tag << std::endl;
		std::cout << "[Free SDFBlocks " << g_sceneRep->getHeapFreeCount() << " ] " << std::endl;

		g_sceneRep->bindDepthCameraTextures(req.depthCameraData);

		// The transformation is set here from the binary file directly. No need to run ICP below.
		mat4f transformation = req.transformation;

		if (transformation[0] == -std::numeric_limits<float>::infinity()) {
			std::cout << "INVALID FRAME" << std::endl;
			return;
		}

		//
		// 2. Perform Volumetric Integration with Voxel Hashing
		//
		if (transformation(0, 0) == -std::numeric_limits<float>::infinity()) {
			std::cout << "!!! TRACKING LOST !!!" << std::endl;
			GlobalAppState::get().s_reconstructionEnabled = false;
			return;
		}

		if (GlobalAppState::get().s_streamingEnabled) {
			if (!GlobalAppState::get().s_streamingAdaptive || g_sceneRep->getHeapFreeCount() < GlobalAppState::get().s_streamingThreshold){
				PROFILE_CODE(profile.startTiming("Streaming", num_processed_frames_));
				vec4f posWorld = transformation*GlobalAppState::getInstance().s_streamingPos; // trans laggs one frame *trans
				vec3f p(posWorld.x, posWorld.y, posWorld.z);

				g_chunkGrid->streamOutToCPUPass0GPU(p, GlobalAppState::get().s_streamingRadius, true, true);
				g_chunkGrid->streamInToGPUPass1GPU(true);

				//g_chunkGrid->debugCheckForDuplicates();
				PROFILE_CODE(profile.stopTiming("Streaming", num_processed_frames_));
			}
		}

		// perform integration
		if (GlobalAppState::get().s_integrationEnabled) {
			PROFILE_CODE(profile.startTiming("Integration", num_processed_frames_));
			g_sceneRep->integrate(transformation, req.depthCameraData, req.depthCameraParams, g_chunkGrid->getBitMaskGPU());
			PROFILE_CODE(profile.stopTiming("Integration", num_processed_frames_));
		}
		else {
			//compactification is required for the raycast splatting
			assert(false);	// guess we should not land here
			g_sceneRep->setLastRigidTransformAndCompactify(transformation, g_CudaDepthSensor.getDepthCameraData());

		}
	}
};


class FrameBasedScheduler : public MultiFrameScheduler{
public:
	void schedule_and_execute() override{
		assert(!requests_.empty());
		size_t count_integrated = 0;

		// Opt0: Optional naive reordering

		if (GlobalAppState::get().s_naiveReorder)
		{
			std::sort(requests_.begin(), requests_.end(), [](FrameRequest &a, FrameRequest &b){return a.sensor_id < b.sensor_id; });
		}

		while (!requests_.empty()){
			num_processed_frames_++;
			size_t chosen_frame = 0;

			// Opt1: Optional smart reordering:
			// Select a frame that's closest to the scene's last transformation
			if (!GlobalAppState::get().s_naiveReorder && GlobalAppState::get().s_smartReorder){
				mat4f last_transform = g_sceneRep->getLastRigidTransform();
				vec4f last_posWorld = last_transform*GlobalAppState::getInstance().s_streamingPos;
				vec4f req_posWorld = requests_[0].transformation*GlobalAppState::getInstance().s_streamingPos;
				float closest_dist = vec4f::dist(last_posWorld, req_posWorld);
				size_t closest_idx = 0;

				for (size_t i = 1; i < requests_.size(); i++){
					vec4f req_posWorld = requests_[i].transformation * GlobalAppState::getInstance().s_streamingPos;
					float d = vec4f::dist(last_posWorld, req_posWorld);
					if (d < closest_dist){
						closest_dist = d;
						closest_idx = i;
					}
				}

			}
			else{
				// Do nothing, equivalent to round robin
				chosen_frame = 0;
			}

			auto req = requests_[chosen_frame];

			// Opt2: Heat-based skipping
			if (GlobalAppState::get().s_skipFrameEnabled){
				auto transformation = req.transformation;
				vec4f posWorld = transformation*GlobalAppState::getInstance().s_streamingPos; // trans laggs one frame *trans
				vec3f p(posWorld.x, posWorld.y, posWorld.z);
				vec3i chunkIdx = g_chunkGrid->worldToChunks(p);

				if (getHeat(chunkIdx.x, chunkIdx.y, chunkIdx.z) > GlobalAppState::get().s_skipFrameThreshold && 1 < requests_.size()){
					// hot enough, skip! (but let it go if the last frame)
					std::cout << "Skipping " << req.tag << std::endl;
					requests_.erase(requests_.begin() + chosen_frame);
					continue;
				}

				incrementHeat(chunkIdx.x, chunkIdx.y, chunkIdx.z);
			}
			execute_frame_request(requests_[chosen_frame]);
			requests_.erase(requests_.begin() + chosen_frame);
		}

		decayHeat();
	}

private:
	struct key_hash {
		size_t operator()(const std::tuple<int, int, int>& k){
			int x, y, z;
			std::tie(x, y, z) = k;
			return std::hash<int>{}(x) ^ std::hash<int>{}(y) ^ std::hash<int>{}(z);
		}
	};
	std::unordered_map<std::tuple<int, int, int>, float, key_hash> heat_map_;
	void incrementHeat(int x, int y, int z){
		auto key = std::make_tuple(x, y, z);
		if (heat_map_.end() == heat_map_.find(key)){
			heat_map_[key] = 1.0f;
		}
		else{
			heat_map_[key] += 1.0f;
		}
	}

	float getHeat(int x, int y, int z){
		auto key = std::make_tuple(x, y, z);
		if (heat_map_.end() == heat_map_.find(key)){
			return 0;
		}
		else{
			return heat_map_[key];
		}
	}

	void decayHeat(){
		for (auto iter : heat_map_){
			iter.second = 0.9f * iter.second;
		}
	}
};


#pragma endregion



RGBDSensor* getRGBDSensor()
{
	static RGBDSensor* g_sensor = NULL;
	if (g_sensor != NULL)	return g_sensor;

	if (GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_Kinect) {
#ifdef KINECT
		g_sensor = new KinectSensor;
		return g_sensor;
#else 
		throw MLIB_EXCEPTION("Requires KINECT V1 SDK and enable KINECT macro");
#endif
	}

	else if (GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_PrimeSense)	{
#ifdef OPEN_NI
		g_sensor = new PrimeSenseSensor;
		return g_sensor;
#else 
		throw MLIB_EXCEPTION("Requires OpenNI 2 SDK and enable OPEN_NI macro");
#endif
	}
	else if (GlobalAppState::getInstance().s_sensorIdx == GlobalAppState::Sensor_KinectOne) {
#ifdef KINECT_ONE
		g_sensor = new KinectOneSensor;
		return g_sensor;
#else
		throw MLIB_EXCEPTION("Requires Kinect 2.0 SDK and enable KINECT_ONE macro");
#endif
	}
	else if (GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_BinaryDumpReader) {
#ifdef BINARY_DUMP_READER
		g_sensor = new BinaryDumpReader(GlobalAppState::get().s_binaryDumpSensorFile);
		return g_sensor;
#else 
		throw MLIB_EXCEPTION("Requires BINARY_DUMP_READER macro");
#endif
	}
	else if (GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_NetworkSensor) {
		g_sensor = new NetworkSensor;
		return g_sensor;
	}
	else if (GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_IntelSensor) {
#ifdef INTEL_SENSOR
		g_sensor = new IntelSensor;
		return g_sensor;
#else 
		throw MLIB_EXCEPTION("Requires INTEL_SENSOR macro");
#endif
	}
	else if (GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_RealSense) {
#ifdef REAL_SENSE
		g_sensor = RealSenseSensor;
		return g_sensor;
#else
		throw MLIB_EXCEPTION("Requires Real Sense SDK and REAL_SENSE macro");
#endif
	}
	else if (GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_StructureSensor) {
#ifdef STRUCTURE_SENSOR
		g_sensor = new StructureSensor;
		return g_sensor;
#else
		throw MLIB_EXCEPTION("Requires STRUCTURE_SENSOR macro");
#endif
	}
	else if (GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_SensorDataReader) {
#ifdef SENSOR_DATA_READER
		g_sensor = new SensorDataReader;
		return g_sensor;
#else
		throw MLIB_EXCEPTION("Requires STRUCTURE_SENSOR macro");
#endif
	}
	else if (GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_MultiSensor) {
#ifdef MULTI_SENSOR
		
		std::string s_filelist = GlobalAppState::get().s_binaryDumpSensorFileList;
		std::istringstream ss(s_filelist);
		std::string token;
		std::vector<std::string> filelist;
		vector<RGBDSensor*> sensors;
		while (std::getline(ss, token, ',')) {
			std::cout << "creating binary dump reader from " << token << "" << std::endl;
			sensors.push_back(new BinaryDumpReader(token));
		}

		g_sensor = new MultiSensor(sensors);
		return g_sensor;
#else
		throw MLIB_EXCEPTION("Requires MULTI_SENSOR macro");
#endif
	}

	throw MLIB_EXCEPTION("unkown sensor id " + std::to_string(GlobalAppState::get().s_sensorIdx));
	return NULL;
}


//--------------------------------------------------------------------------------------
// Initialize the app 
//--------------------------------------------------------------------------------------
void InitApp()
{
}

//--------------------------------------------------------------------------------------
// Called right before creating a D3D9 or D3D10 device, allowing the app to modify the device settings as needed
//--------------------------------------------------------------------------------------
bool CALLBACK ModifyDeviceSettings( DXUTDeviceSettings* pDeviceSettings, void* pUserContext )
{
	// For the first device created if its a REF device, optionally display a warning dialog box
	static bool s_bFirstTime = true;
	if( s_bFirstTime )
	{
		s_bFirstTime = false;
		if( ( DXUT_D3D9_DEVICE == pDeviceSettings->ver && pDeviceSettings->d3d9.DeviceType == D3DDEVTYPE_REF ) ||
			( DXUT_D3D11_DEVICE == pDeviceSettings->ver &&
			pDeviceSettings->d3d11.DriverType == D3D_DRIVER_TYPE_REFERENCE ) )
		{
			DXUTDisplaySwitchingToREFWarning( pDeviceSettings->ver );
		}
	}

	return true;
}

//--------------------------------------------------------------------------------------
// Handle updates to the scene
//--------------------------------------------------------------------------------------
/**
 * This function is called before a frame is rendered. It is used to process the world state. 
 * However, its update frequency depends on the speed of the system. On faster systems, 
 * it is called more often per second. This means that any state update code must be regulated by time. 
 * Otherwise, it performs differently on a slower system than on a faster system. Note that the rendering 
 * calls are not included.
 */
void CALLBACK OnFrameMove( double fTime, float fElapsedTime, void* pUserContext )
{
	g_Camera.FrameMove( fElapsedTime );
	// Update the camera's position based on user input 
}

//--------------------------------------------------------------------------------------
// Render the statistics text
//--------------------------------------------------------------------------------------
void RenderText()
{
	g_pTxtHelper->Begin();
	g_pTxtHelper->SetInsertionPos( 2, 0 );
	g_pTxtHelper->SetForegroundColor( D3DXCOLOR( 1.0f, 1.0f, 0.0f, 1.0f ) );
	g_pTxtHelper->DrawTextLine( DXUTGetFrameStats( DXUTIsVsyncEnabled() ) );
	g_pTxtHelper->DrawTextLine( DXUTGetDeviceStats() );
	if (!g_bRenderHelp) {
		g_pTxtHelper->SetForegroundColor( D3DXCOLOR( 1.0f, 1.0f, 1.0f, 1.0f ) );
		g_pTxtHelper->DrawTextLine(L"\tPress F1 for help");
	}
	g_pTxtHelper->End();


	if (g_bRenderHelp) {
		RenderHelp();
	}
}

void RenderHelp() 
{
	g_pTxtHelper->Begin();
	g_pTxtHelper->SetInsertionPos( 2, 40 );
	g_pTxtHelper->SetForegroundColor( D3DXCOLOR( 1.0f, 0.0f, 0.0f, 1.0f ) );
	g_pTxtHelper->DrawTextLine( L"Controls " );
	g_pTxtHelper->DrawTextLine(L"  \tF1:\t Hide help");
	g_pTxtHelper->DrawTextLine(L"  \tF2:\t Screenshot");
	g_pTxtHelper->DrawTextLine(L"  \t'R':\t Reset scan");
	g_pTxtHelper->DrawTextLine(L"  \t'9':\t Extract geometry (Marching Cubes)");
	g_pTxtHelper->DrawTextLine(L"  \t'8':\t Save recorded input data to sensor file (if enabled)");
	g_pTxtHelper->DrawTextLine(L"  \t'<tab>':\t Switch to free-view mode");
	g_pTxtHelper->DrawTextLine(L"  \t");
	g_pTxtHelper->DrawTextLine(L"  \t'0':\t Visualize re-sampled raw depth data");
	g_pTxtHelper->DrawTextLine(L"  \t'1':\t Visualize reconstruction (default)");
	g_pTxtHelper->DrawTextLine(L"  \t'2':\t Visualize input depth");
	g_pTxtHelper->DrawTextLine(L"  \t'3':\t Visualize input color");
	g_pTxtHelper->DrawTextLine(L"  \t'4':\t Visualize input normals");
	g_pTxtHelper->DrawTextLine(L"  \t'5':\t Visualize phong shaded");
	g_pTxtHelper->DrawTextLine(L"  \t'H':\t GPU hash statistics");
	g_pTxtHelper->DrawTextLine(L"  \t'T':\t Print detailed timings");
	g_pTxtHelper->DrawTextLine(L"  \t'M':\t Debug hash");
	g_pTxtHelper->DrawTextLine(L"  \t'N':\t Save hash to file");
	g_pTxtHelper->DrawTextLine(L"  \t'N':\t Load hash from file");
	g_pTxtHelper->DrawTextLine(L"  \t");
	g_pTxtHelper->End();
}


//--------------------------------------------------------------------------------------
// Handle messages to the application
//--------------------------------------------------------------------------------------
/**
 * DXUT invokes this function when window messages are received. 
 * The function allows the application to handle messages as it
 * sees fit.
 */
LRESULT CALLBACK MsgProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, bool* pbNoFurtherProcessing,
						 void* pUserContext )
{
	// Pass messages to dialog resource manager calls so GUI state is updated correctly
	*pbNoFurtherProcessing = g_DialogResourceManager.MsgProc( hWnd, uMsg, wParam, lParam );
	if( *pbNoFurtherProcessing )
		return 0;

	g_Camera.HandleMessages( hWnd, uMsg, wParam, lParam );

	return 0;
}


void StopScanningAndExtractIsoSurfaceMC(const std::string& filename)
{
	Timer t;

	vec4f posWorld = g_sceneRep->getLastRigidTransform()*GlobalAppState::get().s_streamingPos; // trans lags one frame
	vec3f p(posWorld.x, posWorld.y, posWorld.z);

	g_marchingCubesHashSDF->clearMeshBuffer();
	if (!GlobalAppState::get().s_streamingEnabled) {
		//g_chunkGrid->stopMultiThreading();
		//g_chunkGrid->streamInToGPUAll();
		g_marchingCubesHashSDF->extractIsoSurface(g_sceneRep->getHashData(), g_sceneRep->getHashParams(), g_rayCast->getRayCastData());
		//g_chunkGrid->startMultiThreading();
	} else {
		g_marchingCubesHashSDF->extractIsoSurface(*g_chunkGrid, g_rayCast->getRayCastData(), p, GlobalAppState::getInstance().s_streamingRadius);
	}

	const mat4f& rigidTransform = mat4f::identity();//g_sceneRep->getLastRigidTransform();
	g_marchingCubesHashSDF->saveMesh(filename, &rigidTransform);

	std::cout << "Mesh generation time " << t.getElapsedTime() << " seconds" << std::endl;

	//g_sceneRep->debugHash();
	//g_chunkGrid->debugCheckForDuplicates();
}

void ResetDepthSensing()
{
	g_sceneRep->reset();
	g_RGBDAdapter.reset();
	g_chunkGrid->reset();
	g_Camera.Reset();
}


void StopScanningAndSaveSDFHash(const std::string& filename = "test.hashgrid") {
	//g_sceneRep->debugHash();
	//g_chunkGrid->debugCheckForDuplicates();

	Timer t;
	std::cout << "saving hash grid to file " << filename << "... ";

	vec4f posWorld = g_sceneRep->getLastRigidTransform()*GlobalAppState::get().s_streamingPos; // trans lags one frame
	vec3f p(posWorld.x, posWorld.y, posWorld.z);

	g_chunkGrid->saveToFile(filename, g_rayCast->getRayCastData(), p, GlobalAppState::getInstance().s_streamingRadius);

	std::cout << "Done!" << std::endl;
	std::cout << "Saving Time " << t.getElapsedTime() << " seconds" << std::endl;

	//g_sceneRep->debugHash();
	//g_chunkGrid->debugCheckForDuplicates();
}


void StopScanningAndLoadSDFHash(const std::string& filename = "test.hashgrid") {
	//g_sceneRep->debugHash();
	//g_chunkGrid->debugCheckForDuplicates();

	Timer t;

	vec4f posWorld = g_sceneRep->getLastRigidTransform()*GlobalAppState::get().s_streamingPos; // trans lags one frame
	vec3f p(posWorld.x, posWorld.y, posWorld.z);

	ResetDepthSensing();
	g_chunkGrid->loadFromFile(filename, g_rayCast->getRayCastData(), p, GlobalAppState::getInstance().s_streamingRadius);

	std::cout << "Loading Time " << t.getElapsedTime() << " seconds" << std::endl;

	GlobalAppState::get().s_integrationEnabled = false;
	std::cout << "Integration enabled == false" << std::endl; 
	GlobalAppState::get().s_trackingEnabled = false;
	std::cout << "Tracking enabled == false" << std::endl;

	//g_sceneRep->debugHash();
	//g_chunkGrid->debugCheckForDuplicates();
}

//--------------------------------------------------------------------------------------
// Handle key presses
//--------------------------------------------------------------------------------------
static int whichScreenshot = 0;


void CALLBACK OnKeyboard( UINT nChar, bool bKeyDown, bool bAltDown, void* pUserContext )
{

	if( bKeyDown ) {
		wchar_t sz[200];

		switch( nChar )
		{
		case VK_F1:
			g_bRenderHelp = !g_bRenderHelp;
			break;
		case VK_F2:
			swprintf_s(sz, 200, L"screenshot%d.bmp", whichScreenshot++);
			DXUTSnapD3D11Screenshot(sz, D3DX11_IFF_BMP);
			std::wcout << std::wstring(sz) << std::endl;
			break;
		case '\t':
			g_renderText = !g_renderText;
			break;
		case '1':
			GlobalAppState::get().s_RenderMode = 1;
			break;
		case '2':
			GlobalAppState::get().s_RenderMode = 2;
			break;
		case '3':
			GlobalAppState::get().s_RenderMode = 3;
			break;
		case '4':
			GlobalAppState::get().s_RenderMode = 4;
			break;
		case '5':
			GlobalAppState::get().s_RenderMode = 5;
			break;
		case '6':
			GlobalAppState::get().s_RenderMode = 6;
			break;
		case '7':
			GlobalAppState::get().s_RenderMode = 7;
			break;
			//case '8':
			//GlobalAppState::get().s_RenderMode = 8;
		case '8':
			{
				if (GlobalAppState::getInstance().s_recordData) {
					g_RGBDAdapter.saveRecordedFramesToFile(GlobalAppState::getInstance().s_recordDataFile);
				} else {
					std::cout << "Cannot save recording: enable \"s_recordData\" in parameter file" << std::endl;
				}
				break;
			}
			break;
		case '9':
			StopScanningAndExtractIsoSurfaceMC();
			break;
		case '0':
			GlobalAppState::get().s_RenderMode = 0;
			break;
		case 'T':
			GlobalAppState::get().s_timingsDetailledEnabled = !GlobalAppState::get().s_timingsDetailledEnabled;
			break;
		case 'Z':
			GlobalAppState::get().s_timingsTotalEnabled = !GlobalAppState::get().s_timingsTotalEnabled;
			break;
			//case VK_F3:
			//	GlobalAppState::get().s_texture_threshold += 0.02;
			//	std::cout<<GlobalAppState::get().s_texture_threshold<<std::endl;
			//	if(GlobalAppState::get().s_texture_threshold>1.0f)
			//		GlobalAppState::get().s_texture_threshold = 1.0f;
			//	break;
			//case VK_F4:
			//	GlobalAppState::get().s_texture_threshold -= 0.02;
			//	std::cout<<GlobalAppState::get().s_texture_threshold<<std::endl;
			//	if(GlobalAppState::get().s_texture_threshold<0.0f)
			//		GlobalAppState::get().s_texture_threshold = 0.0f;
			//	break;
		case 'R':
			ResetDepthSensing();
			break;
		case 'H':
			g_historgram->computeHistrogram(g_sceneRep->getHashData(), g_sceneRep->getHashParams());
			break;
		case 'M':
			g_sceneRep->debugHash();
			if (g_chunkGrid)	g_chunkGrid->debugCheckForDuplicates();
			break;
		case 'L':
			g_RGBDAdapter.getRGBDSensor()->savePointCloud("test.ply");
			break;
		case 'Y':
			{
				float* h_rawDepth = g_RGBDAdapter.getRGBDSensor()->getDepthFloat();
				DepthImage dRawImage(g_RGBDAdapter.getRGBDSensor()->getDepthHeight(), g_RGBDAdapter.getRGBDSensor()->getDepthWidth(), h_rawDepth);
				ColorImageRGB cRawImage(dRawImage);
				FreeImageWrapper::saveImage("raw.png", cRawImage);

				Util::writeToImage(g_RGBDAdapter.getRawDepthMap(), g_RGBDAdapter.getRGBDSensor()->getDepthWidth(), g_RGBDAdapter.getRGBDSensor()->getDepthHeight(), "aRaw.png");
				Util::writeToImage(g_RGBDAdapter.getDepthMapResampledFloat(), g_RGBDAdapter.getWidth(), g_RGBDAdapter.getHeight(), "aResampled.png");
				Util::writeToImage(g_CudaDepthSensor.getDepthCameraData().d_depthData, g_CudaDepthSensor.getDepthCameraParams().m_imageWidth, g_CudaDepthSensor.getDepthCameraParams().m_imageHeight, "depth.png");
				Util::writeToImage(g_rayCast->getRayCastData().d_depth, g_rayCast->getRayCastParams().m_width, g_rayCast->getRayCastParams().m_height, "raycast.png");

				break;
			}
		case 'N':
			StopScanningAndSaveSDFHash("test.hashgrid");
			break;
		case 'B':
			StopScanningAndLoadSDFHash("test.hashgrid");
			break;
		case 'I':
			GlobalAppState::get().s_integrationEnabled = !GlobalAppState::get().s_integrationEnabled;
			if (GlobalAppState::get().s_integrationEnabled)		std::cout << "integration enabled" << std::endl;
			else std::cout << "integration disabled" << std::endl;
			break;
		case 'P':
			profile.generateTimingStats();
			profile.printTimingStats();
		case 'Q':
			std::cout << "dumping profiling result...";
			profile.dumpToFolderAll(GlobalAppState::get().s_profilerDumpFolder);
			std::cout << "done." << std::endl;
			break;
		case VK_ESCAPE:
			exit(0);
			break;
		default:
			break;
		}
	}
}

//--------------------------------------------------------------------------------------
// Handles the GUI events
//--------------------------------------------------------------------------------------
void CALLBACK OnGUIEvent( UINT nEvent, int nControlID, CDXUTControl* pControl, void* pUserContext )
{
	switch( nControlID )
	{
		// Standard DXUT controls
	case IDC_TOGGLEFULLSCREEN:
		DXUTToggleFullScreen(); 
		break;
	case IDC_TOGGLEREF:
		DXUTToggleREF(); 
		break;
	case IDC_TEST:
		break;
	}
}

//--------------------------------------------------------------------------------------
// Reject any D3D11 devices that aren't acceptable by returning false
//--------------------------------------------------------------------------------------
bool CALLBACK IsD3D11DeviceAcceptable( const CD3D11EnumAdapterInfo *AdapterInfo, UINT Output, const CD3D11EnumDeviceInfo *DeviceInfo, DXGI_FORMAT BackBufferFormat, bool bWindowed, void* pUserContext )
{
	return true;
}

//--------------------------------------------------------------------------------------
// Create any D3D11 resources that aren't dependent on the back buffer
//--------------------------------------------------------------------------------------
HRESULT CALLBACK OnD3D11CreateDevice(ID3D11Device* pd3dDevice, const DXGI_SURFACE_DESC* pBackBufferSurfaceDesc, void* pUserContext)
{
	HRESULT hr = S_OK;

	V_RETURN(GlobalAppState::get().OnD3D11CreateDevice(pd3dDevice));

	ID3D11DeviceContext* pd3dImmediateContext = DXUTGetD3D11DeviceContext();

	V_RETURN( g_DialogResourceManager.OnD3D11CreateDevice( pd3dDevice, pd3dImmediateContext ) );
	g_pTxtHelper = new CDXUTTextHelper( pd3dDevice, pd3dImmediateContext, &g_DialogResourceManager, 15 );

	if (getRGBDSensor() == NULL)
	{
		std::cout << "No RGBD Sensor specified" << std::endl;
		while(1);
	}

	if ( FAILED( getRGBDSensor()->createFirstConnected() ) )
	{
		MessageBox(NULL, L"No ready Depth Sensor found!", L"Error", MB_ICONHAND | MB_OK);
		return S_FALSE;
	}

	V_RETURN(g_RGBDAdapter.OnD3D11CreateDevice(pd3dDevice, getRGBDSensor(), GlobalAppState::get().s_adapterWidth, GlobalAppState::get().s_adapterHeight));
	V_RETURN(g_CudaDepthSensor.OnD3D11CreateDevice(pd3dDevice, &g_RGBDAdapter));

	V_RETURN(DX11QuadDrawer::OnD3D11CreateDevice(pd3dDevice));
	V_RETURN(DX11PhongLighting::OnD3D11CreateDevice(pd3dDevice));

	TimingLog::init();

	std::vector<DXGI_FORMAT> formats;
	formats.push_back(DXGI_FORMAT_R32_FLOAT);
	formats.push_back(DXGI_FORMAT_R32G32B32A32_FLOAT);
	formats.push_back(DXGI_FORMAT_R32G32B32A32_FLOAT);
	formats.push_back(DXGI_FORMAT_R32G32B32A32_FLOAT);

	V_RETURN(g_RGBDRenderer.OnD3D11CreateDevice(pd3dDevice, GlobalAppState::get().s_adapterWidth, GlobalAppState::get().s_adapterHeight));
	V_RETURN(g_CustomRenderTarget.OnD3D11CreateDevice(pd3dDevice, GlobalAppState::get().s_adapterWidth, GlobalAppState::get().s_adapterHeight, formats));

	D3DXVECTOR3 vecEye ( 0.0f, 0.0f, 0.0f );
	D3DXVECTOR3 vecAt ( 0.0f, 0.0f, 1.0f );
	g_Camera.SetViewParams( &vecEye, &vecAt );

	g_cameraTracking = new CUDACameraTrackingMultiRes(g_RGBDAdapter.getWidth(), g_RGBDAdapter.getHeight(), GlobalCameraTrackingState::get().s_maxLevels);
	//g_cameraTrackingRGBD = new CUDACameraTrackingMultiResRGBD(g_RGBDAdapter.getWidth(), g_RGBDAdapter.getHeight(), GlobalCameraTrackingState::get().s_maxLevels);

	//g_CUDASolverSFS = new CUDAPatchSolverSFS();
	//g_CUDASolverSHLighting = new CUDASolverSHLighting(GlobalAppState::get().s_adapterWidth, GlobalAppState::get().s_adapterHeight);

	g_sceneRep = new CUDASceneRepHashSDF(CUDASceneRepHashSDF::parametersFromGlobalAppState(GlobalAppState::get()));
	g_rayCast = new CUDARayCastSDF(CUDARayCastSDF::parametersFromGlobalAppState(GlobalAppState::get(), g_RGBDAdapter.getColorIntrinsics(), g_RGBDAdapter.getColorIntrinsicsInv()));

	g_marchingCubesHashSDF = new CUDAMarchingCubesHashSDF(CUDAMarchingCubesHashSDF::parametersFromGlobalAppState(GlobalAppState::get()));
	g_historgram = new CUDAHistrogramHashSDF(g_sceneRep->getHashParams());

	g_chunkGrid = new CUDASceneRepChunkGrid(g_sceneRep, 
		GlobalAppState::get().s_streamingChunkExtents, 
		GlobalAppState::get().s_streamingGridDimensions,
		GlobalAppState::get().s_streamingMinGridPos,
		GlobalAppState::get().s_streamingInitialChunkListSize,
		GlobalAppState::get().s_streamingEnabled,
		GlobalAppState::get().s_streamingOutParts);

	// bind the array to texture for efficiency
	if (g_CudaDepthSensor.getMode() == NoBuffering) {
		g_sceneRep->bindDepthCameraTextures(g_CudaDepthSensor.getDepthCameraData());
	}

	if (!GlobalAppState::get().s_reconstructionEnabled) {
		GlobalAppState::get().s_RenderMode = 2;
	}

	return hr;
}

//--------------------------------------------------------------------------------------
// Release D3D11 resources created in OnD3D10CreateDevice 
//--------------------------------------------------------------------------------------
void CALLBACK OnD3D11DestroyDevice( void* pUserContext )
{
	g_DialogResourceManager.OnD3D11DestroyDevice();
	DXUTGetGlobalResourceCache().OnDestroyDevice();
	SAFE_DELETE( g_pTxtHelper );

	DX11QuadDrawer::OnD3D11DestroyDevice();
	DX11PhongLighting::OnD3D11DestroyDevice();
	GlobalAppState::get().OnD3D11DestroyDevice();

	g_CudaDepthSensor.OnD3D11DestroyDevice();
	g_RGBDAdapter.OnD3D11DestroyDevice();
	g_RGBDRenderer.OnD3D11DestroyDevice();
	g_CustomRenderTarget.OnD3D11DestroyDevice();

	SAFE_DELETE(g_cameraTracking);
	SAFE_DELETE(g_cameraTrackingRGBD);

	SAFE_DELETE(g_sceneRep);
	SAFE_DELETE(g_rayCast);
	SAFE_DELETE(g_marchingCubesHashSDF);
	SAFE_DELETE(g_historgram);
	SAFE_DELETE(g_chunkGrid);

	TimingLog::destroy();
}

//--------------------------------------------------------------------------------------
// Create any D3D11 resources that depend on the back buffer
//--------------------------------------------------------------------------------------
HRESULT CALLBACK OnD3D11ResizedSwapChain( ID3D11Device* pd3dDevice, IDXGISwapChain* pSwapChain,
										 const DXGI_SURFACE_DESC* pBackBufferSurfaceDesc, void* pUserContext )
{
	HRESULT hr = S_OK;

	V_RETURN( g_DialogResourceManager.OnD3D11ResizedSwapChain( pd3dDevice, pBackBufferSurfaceDesc ) );

	// Setup the camera's projection parameters
	g_Camera.SetWindow( pBackBufferSurfaceDesc->Width, pBackBufferSurfaceDesc->Height );
	g_Camera.SetButtonMasks( MOUSE_MIDDLE_BUTTON, MOUSE_WHEEL, MOUSE_LEFT_BUTTON );

	//g_Camera.SetRotateButtons(true, false, false);

	float fAspectRatio = pBackBufferSurfaceDesc->Width / ( FLOAT )pBackBufferSurfaceDesc->Height;
	//D3DXVECTOR3 vecEye ( 0.0f, 0.0f, 0.0f );
	//D3DXVECTOR3 vecAt ( 0.0f, 0.0f, 1.0f );
	//g_Camera.SetViewParams( &vecEye, &vecAt );
	g_Camera.SetProjParams( D3DX_PI / 4, fAspectRatio, 0.1f, 10.0f );


	V_RETURN(DX11PhongLighting::OnResize(pd3dDevice, pBackBufferSurfaceDesc->Width, pBackBufferSurfaceDesc->Height));

	return hr;
}

//--------------------------------------------------------------------------------------
// Release D3D11 resources created in OnD3D10ResizedSwapChain 
//--------------------------------------------------------------------------------------
void CALLBACK OnD3D11ReleasingSwapChain( void* pUserContext )
{
	g_DialogResourceManager.OnD3D11ReleasingSwapChain();
}


/**
* 15769: Entry point for reconstruction procedure for multi binary dump.
* Everything goes in here.
*/
//MultiFrameScheduler scheduler;
FrameBasedScheduler scheduler;

void reconstruction_multi_dump(){
	assert(GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_MultiSensor);
	assert(GlobalAppState::get().s_binaryDumpSensorUseTrajectory);

#ifdef MULTI_SENSOR
	// schedule
	std::cout << "Fetching a new batch ..." << std::endl;
	vector<FrameEntry> frames = g_CudaDepthSensor.getBufferedFrame();
	if (g_CudaDepthSensor.getMode() == NoBuffering) {
		int id = 0;
		scheduler.add_request(MultiFrameScheduler::FrameRequest(
			g_CudaDepthSensor.getRigidTransform(),
			g_CudaDepthSensor.getDepthCameraData(),
			g_CudaDepthSensor.getDepthCameraParams(),
			id,
			std::string("Sensor ") + std::to_string(g_RGBDAdapter.getCurrentSensorIdx()) + std::string(", Frame ") + std::to_string(g_RGBDAdapter.getFrameNumber())));
	}
	else if (g_CudaDepthSensor.getMode() == BatchBuffering) {
		for (int i = 0; i < frames.size(); i++)
		{
			scheduler.add_request(MultiFrameScheduler::FrameRequest(
				frames[i].rigidTransformation,
				frames[i].depthCameraData,
				g_CudaDepthSensor.getDepthCameraParams(),
				frames[i].sensorId,
				std::string("Sensor ") + std::to_string(frames[i].sensorId) + std::string(", Frame ") + std::to_string(g_RGBDAdapter.getFrameNumber())));
		}
	}

	int numRequest = scheduler.num_request();
	if (numRequest > 0) {
		// integrate
		scheduler.schedule_and_execute();
		// render
		if (g_CudaDepthSensor.getMode() == NoBuffering) {
			g_rayCast->render(g_sceneRep->getHashData(), g_sceneRep->getHashParams(),
				g_CudaDepthSensor.getDepthCameraData(), g_CudaDepthSensor.getRigidTransform());
		}
		else if (g_CudaDepthSensor.getMode() == BatchBuffering) {
			vector<FrameEntry> frames = g_CudaDepthSensor.getBufferedFrame();
			g_rayCast->render(g_sceneRep->getHashData(), g_sceneRep->getHashParams(),
				frames[0].depthCameraData, frames[0].rigidTransformation);
		}
	}
#endif
}

/**
 * The entry point for the 3d reconstruction procedure
 */
void reconstruction()
{
	if (GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_MultiSensor)
	{
		return reconstruction_multi_dump();
	}

	//only if binary dump or multi-sensor (because currently multi sensor only supports multiple binary readers)
	if (GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_BinaryDumpReader || GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_SensorDataReader) {
		std::cout << "[ frame " << g_RGBDAdapter.getFrameNumber() << " ] " << " [Free SDFBlocks " << g_sceneRep->getHeapFreeCount() << " ] " << std::endl;
	}

	mat4f transformation = mat4f::identity();
	if ((GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_BinaryDumpReader || GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_SensorDataReader)
		&& GlobalAppState::get().s_binaryDumpSensorUseTrajectory) {

		// The transformation is set here from the binary file directly. No need to run ICP below.
		transformation = g_RGBDAdapter.getRigidTransform();

		if (transformation[0] == -std::numeric_limits<float>::infinity()) {
			std::cout << "INVALID FRAME" << std::endl;
			return;
		}
	}

	//
	// Perform ICP for tracking
	// After this step, transformation matrix is set.
	//
#pragma region ignored

	if (true) {
		mat4f renderTransform = g_sceneRep->getLastRigidTransform();

		//if we have a pre-recorded trajectory; use it as an init (if specificed to do so)
		if ((GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_BinaryDumpReader || GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_SensorDataReader)
			&& GlobalAppState::get().s_binaryDumpSensorUseTrajectory
			&& GlobalAppState::get().s_binaryDumpSensorUseTrajectoryOnlyInit) {
			//deltaTransformEstimate = lastTransform.getInverse() * transformation;
			mat4f deltaTransformEstimate = g_RGBDAdapter.getRigidTransform(-1).getInverse() * transformation;
			renderTransform = renderTransform * deltaTransformEstimate;
			g_sceneRep->setLastRigidTransformAndCompactify(renderTransform, g_CudaDepthSensor.getDepthCameraData());
			//TODO if this is enabled there is a problem with the ray interval splatting
		}

		g_rayCast->render(g_sceneRep->getHashData(), g_sceneRep->getHashParams(), g_CudaDepthSensor.getDepthCameraData(), renderTransform);
		if (GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_NetworkSensor)
		{
			mat4f rigid_transform_from_tango = g_RGBDAdapter.getRigidTransform();
			transformation = rigid_transform_from_tango;
			if (transformation(0, 0) == -std::numeric_limits<float>::infinity()) {
				std::cout << "Tracking lost in DepthSensing..." << std::endl;
				transformation = rigid_transform_from_tango;
			}
		}
		else
		{
			if (!GlobalAppState::get().s_trackingEnabled || g_RGBDAdapter.getFrameNumber() == 1) {
				transformation.setIdentity();
			}
			else if (
				(GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_BinaryDumpReader || GlobalAppState::get().s_sensorIdx == GlobalAppState::Sensor_SensorDataReader)
				&& GlobalAppState::get().s_binaryDumpSensorUseTrajectory
				&& !GlobalAppState::get().s_binaryDumpSensorUseTrajectoryOnlyInit) {
				//actually: nothing to do here; transform is already set: just don't do icp and use pre-recorded trajectory
			}
			else {
				mat4f lastTransform = g_sceneRep->getLastRigidTransform();
				mat4f deltaTransformEstimate = mat4f::identity();

				const bool useRGBDTracking = false;	//Depth vs RGBD
				PROFILE_CODE(profile.startTiming("ICP Tracking", g_RGBDAdapter.getFrameNumber()));
				if (!useRGBDTracking) {
					transformation = g_cameraTracking->applyCT(
						g_CudaDepthSensor.getCameraSpacePositionsFloat4(), g_CudaDepthSensor.getNormalMapFloat4(), g_CudaDepthSensor.getColorMapFilteredFloat4(),
						//g_rayCast->getRayCastData().d_depth4Transformed, g_CudaDepthSensor.getNormalMapNoRefinementFloat4(), g_CudaDepthSensor.getColorMapFilteredFloat4(),
						g_rayCast->getRayCastData().d_depth4, g_rayCast->getRayCastData().d_normals, g_rayCast->getRayCastData().d_colors,
						lastTransform,
						GlobalCameraTrackingState::getInstance().s_maxInnerIter, GlobalCameraTrackingState::getInstance().s_maxOuterIter,
						GlobalCameraTrackingState::getInstance().s_distThres, GlobalCameraTrackingState::getInstance().s_normalThres,
						100.0f, 3.0f,
						deltaTransformEstimate,
						GlobalCameraTrackingState::getInstance().s_residualEarlyOut,
						g_RGBDAdapter.getDepthIntrinsics(), g_CudaDepthSensor.getDepthCameraData(),
						NULL);
				}
				else {
					transformation = g_cameraTrackingRGBD->applyCT(
						//g_rayCast->getRayCastData().d_depth4Transformed, g_CudaDepthSensor.getColorMapFilteredFloat4(),
						g_CudaDepthSensor.getCameraSpacePositionsFloat4(), g_CudaDepthSensor.getNormalMapFloat4(), g_CudaDepthSensor.getColorMapFilteredFloat4(),
						g_rayCast->getRayCastData().d_depth4, g_rayCast->getRayCastData().d_normals, g_rayCast->getRayCastData().d_colors, //g_CudaDepthSensor.getColorMapFilteredLastFrameFloat4(), // g_rayCast->getRayCastData().d_colors,
						lastTransform,
						GlobalCameraTrackingState::getInstance().s_maxInnerIter, GlobalCameraTrackingState::getInstance().s_maxOuterIter,
						GlobalCameraTrackingState::getInstance().s_distThres, GlobalCameraTrackingState::getInstance().s_normalThres,
						GlobalCameraTrackingState::getInstance().s_colorGradientMin, GlobalCameraTrackingState::getInstance().s_colorThres,
						100.0f, 3.0f,
						deltaTransformEstimate,
						GlobalCameraTrackingState::getInstance().s_weightsDepth,
						GlobalCameraTrackingState::getInstance().s_weightsColor,
						GlobalCameraTrackingState::getInstance().s_residualEarlyOut,
						g_RGBDAdapter.getDepthIntrinsics(), g_CudaDepthSensor.getDepthCameraData(),
						NULL);
				}
				PROFILE_CODE(profile.stopTiming("ICP Tracking", g_RGBDAdapter.getFrameNumber()));
			}
		}
	}
#pragma endregion

	std::cout << transformation << std::endl;

	if (GlobalAppState::getInstance().s_recordData) {
		g_RGBDAdapter.recordTrajectory(transformation);
	}

	if (transformation(0, 0) == -std::numeric_limits<float>::infinity()) {
		std::cout << "!!! TRACKING LOST !!!" << std::endl;
		GlobalAppState::get().s_reconstructionEnabled = false;
		return;
	}

	//
	// Streaming
	// Bidirectional Host-Device Data Streaming 
	//

	if (GlobalAppState::get().s_streamingEnabled) {
		PROFILE_CODE(profile.startTiming("Streaming", g_RGBDAdapter.getFrameNumber()));
		vec4f posWorld = transformation*GlobalAppState::getInstance().s_streamingPos; // center of the active region
		vec3f p(posWorld.x, posWorld.y, posWorld.z);

		g_chunkGrid->streamOutToCPUPass0GPU(p, GlobalAppState::get().s_streamingRadius, true, true);
		g_chunkGrid->streamInToGPUPass1GPU(true);

		//g_chunkGrid->debugCheckForDuplicates();
		PROFILE_CODE(profile.stopTiming("Streaming", g_RGBDAdapter.getFrameNumber()));
	}

	// heap debug
	static int scount = 0;
	const int gap = 50;
	if (scount == gap - 1) {
		g_sceneRep->checkHeapValRange();
	}
	scount = (scount + 1) % gap;


	//
	// Integration
	//

	// perform integration
	if (GlobalAppState::get().s_integrationEnabled) {
		PROFILE_CODE(profile.startTiming("Integration", g_RGBDAdapter.getFrameNumber()));
		g_sceneRep->integrate(transformation, g_CudaDepthSensor.getDepthCameraData(), g_CudaDepthSensor.getDepthCameraParams(), g_chunkGrid->getBitMaskGPU());
		PROFILE_CODE(profile.stopTiming("Integration", g_RGBDAdapter.getFrameNumber()));
	}
	else {
		//compactification is required for the raycast splatting
		g_sceneRep->setLastRigidTransformAndCompactify(transformation, g_CudaDepthSensor.getDepthCameraData());
	}
}

//--------------------------------------------------------------------------------------
// Render the scene using the D3D11 device
//--------------------------------------------------------------------------------------
/**
 * This function is called whenever a frame is redrawn. Within this function, effects are applied, 
 * resources are associated, and the drawing for the scene is called.
 */
void CALLBACK OnD3D11FrameRender( ID3D11Device* pd3dDevice, ID3D11DeviceContext* pd3dImmediateContext, double fTime, float fElapsedTime, void* pUserContext )
{
	// Clear the back buffer
	static float ClearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	ID3D11RenderTargetView* pRTV = DXUTGetD3D11RenderTargetView();
	ID3D11DepthStencilView* pDSV = DXUTGetD3D11DepthStencilView();
	pd3dImmediateContext->ClearRenderTargetView(pRTV, ClearColor);
	pd3dImmediateContext->ClearDepthStencilView(pDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);

	// if we have received any valid new depth data we may need to draw
	// 15769 Process the whole array of CudaSensor
	HRESULT bGotDepth = S_OK;
	bGotDepth = g_CudaDepthSensor.process(pd3dImmediateContext);

	// Filtering
	g_CudaDepthSensor.setFiterDepthValues(GlobalAppState::get().s_depthFilter, GlobalAppState::get().s_depthSigmaD, GlobalAppState::get().s_depthSigmaR);
	g_CudaDepthSensor.setFiterIntensityValues(GlobalAppState::get().s_colorFilter, GlobalAppState::get().s_colorSigmaD, GlobalAppState::get().s_colorSigmaR);

	HRESULT hr = S_OK;

	///////////////////////////////////////
	// Render
	///////////////////////////////////////

	//Start Timing
	if (GlobalAppState::get().s_timingsDetailledEnabled) { GlobalAppState::get().WaitForGPU(); GlobalAppState::get().s_Timer.start(); }


	mat4f view = MatrixConversion::toMlib(*g_Camera.GetViewMatrix());

	mat4f t = mat4f::identity();
	t(1,1) *= -1.0f;	view = t * view * t;	//t is self-inverse

	if (bGotDepth == S_OK) {
		if (GlobalAppState::getInstance().s_recordData) {
			g_RGBDAdapter.recordFrame();
			if (!GlobalAppState::get().s_reconstructionEnabled) {
				g_RGBDAdapter.recordTrajectory(mat4f::zero());
			}
		}

		if (GlobalAppState::get().s_reconstructionEnabled) {
			reconstruction();
		}
	}

	if(GlobalAppState::get().s_RenderMode == 0) {
		const mat4f renderIntrinsics = g_RGBDAdapter.getColorIntrinsics();

		g_CustomRenderTarget.Clear(pd3dImmediateContext);
		g_CustomRenderTarget.Bind(pd3dImmediateContext);
		g_RGBDRenderer.RenderDepthMap(pd3dImmediateContext, g_CudaDepthSensor.getDepthMapRawFloat(), g_CudaDepthSensor.getColorMapFilteredFloat4(), g_RGBDAdapter.getWidth(), g_RGBDAdapter.getHeight(), g_RGBDAdapter.getColorIntrinsicsInv(), view, renderIntrinsics, g_CustomRenderTarget.getWidth(), g_CustomRenderTarget.getHeight(), GlobalAppState::get().s_renderingDepthDiscontinuityThresOffset, GlobalAppState::get().s_renderingDepthDiscontinuityThresLin);
		g_CustomRenderTarget.Unbind(pd3dImmediateContext);
		DX11PhongLighting::render(pd3dImmediateContext, g_CustomRenderTarget.GetSRV(1), g_CustomRenderTarget.GetSRV(2), g_CustomRenderTarget.GetSRV(3), GlobalAppState::get().s_useColorForRendering, g_CustomRenderTarget.getWidth(), g_CustomRenderTarget.getHeight());
		DX11QuadDrawer::RenderQuad(pd3dImmediateContext, DX11PhongLighting::GetColorsSRV(), 1.0f);
	}
	else if(GlobalAppState::get().s_RenderMode == 1)
	{
		//default render mode
		const mat4f& renderIntrinsics = g_RGBDAdapter.getColorIntrinsics();

		//always render, irrespective whether there is a new depth frame available
		// TODO anything need to change?
		g_CustomRenderTarget.Clear(pd3dImmediateContext);
		g_CustomRenderTarget.Bind(pd3dImmediateContext);
		g_RGBDRenderer.RenderDepthMap(pd3dImmediateContext, g_rayCast->getRayCastData().d_depth, g_rayCast->getRayCastData().d_colors, g_rayCast->getRayCastParams().m_width, g_rayCast->getRayCastParams().m_height, MatrixConversion::toMlib(g_rayCast->getRayCastParams().m_intrinsicsInverse), view, renderIntrinsics, g_CustomRenderTarget.getWidth(), g_CustomRenderTarget.getHeight(), GlobalAppState::get().s_renderingDepthDiscontinuityThresOffset, GlobalAppState::get().s_renderingDepthDiscontinuityThresLin);
		g_CustomRenderTarget.Unbind(pd3dImmediateContext);

		DX11PhongLighting::render(pd3dImmediateContext, g_CustomRenderTarget.GetSRV(1), g_CustomRenderTarget.GetSRV(2), g_CustomRenderTarget.GetSRV(3), GlobalAppState::get().s_useColorForRendering, g_CustomRenderTarget.getWidth(), g_CustomRenderTarget.getHeight());		
		DX11QuadDrawer::RenderQuad(pd3dImmediateContext, DX11PhongLighting::GetColorsSRV(), 1.0f);
	}
	else if(GlobalAppState::get().s_RenderMode == 2) {
		DX11QuadDrawer::RenderQuadDynamic(DXUTGetD3D11Device(), pd3dImmediateContext, (float*)g_CudaDepthSensor.getAndComputeDepthHSV(), 4, g_CudaDepthSensor.getColorWidth(), g_CudaDepthSensor.getColorHeight());
	}
	else if(GlobalAppState::get().s_RenderMode == 3) {
		DX11QuadDrawer::RenderQuadDynamic(DXUTGetD3D11Device(), pd3dImmediateContext, (float*)g_CudaDepthSensor.getColorMapFilteredFloat4(), 4, g_CudaDepthSensor.getColorWidth(), g_CudaDepthSensor.getColorHeight());
	}
	else if(GlobalAppState::get().s_RenderMode == 4) {
		DX11QuadDrawer::RenderQuadDynamic(DXUTGetD3D11Device(), pd3dImmediateContext, (float*)g_CudaDepthSensor.getNormalMapFloat4(), 4, g_CudaDepthSensor.getColorWidth(), g_CudaDepthSensor.getColorHeight());
	}
	else if(GlobalAppState::get().s_RenderMode == 5) {
		//default render mode
		const mat4f& renderIntrinsics = g_RGBDAdapter.getColorIntrinsics();

		g_CustomRenderTarget.Clear(pd3dImmediateContext);
		g_CustomRenderTarget.Bind(pd3dImmediateContext);
		g_RGBDRenderer.RenderDepthMap(pd3dImmediateContext, g_rayCast->getRayCastData().d_depth, g_rayCast->getRayCastData().d_colors, g_rayCast->getRayCastParams().m_width, g_rayCast->getRayCastParams().m_height, MatrixConversion::toMlib(g_rayCast->getRayCastParams().m_intrinsicsInverse), view, renderIntrinsics, g_CustomRenderTarget.getWidth(), g_CustomRenderTarget.getHeight(), GlobalAppState::get().s_renderingDepthDiscontinuityThresOffset, GlobalAppState::get().s_renderingDepthDiscontinuityThresLin);
		g_CustomRenderTarget.Unbind(pd3dImmediateContext);

		DX11PhongLighting::render(pd3dImmediateContext, g_CustomRenderTarget.GetSRV(1), g_CustomRenderTarget.GetSRV(2), g_CustomRenderTarget.GetSRV(3), !GlobalAppState::get().s_useColorForRendering, g_CustomRenderTarget.getWidth(), g_CustomRenderTarget.getHeight());
		DX11QuadDrawer::RenderQuad(pd3dImmediateContext, DX11PhongLighting::GetColorsSRV(), 1.0f);
	}
	else if(GlobalAppState::get().s_RenderMode == 6) {
		//DX11QuadDrawer::RenderQuadDynamic(DXUTGetD3D11Device(), pd3dImmediateContext, (float*)g_rayCast->getRayCastData().d_depth4DV, 4, g_rayCast->getRayCastParams().m_width, g_rayCast->getRayCastParams().m_height, 500.0f);	
	}
	else if(GlobalAppState::get().s_RenderMode == 8) {
		//DX11QuadDrawer::RenderQuadDynamic(DXUTGetD3D11Device(), pd3dImmediateContext, (float*)g_CudaDepthSensor.getColorMapFilteredLastFrameFloat4(), 4, g_CudaDepthSensor.getColorWidth(), g_CudaDepthSensor.getColorHeight());
	}
	else if(GlobalAppState::get().s_RenderMode == 9) {
		const mat4f& renderIntrinsics = g_RGBDAdapter.getColorIntrinsics();

		g_CustomRenderTarget.Clear(pd3dImmediateContext);
		g_CustomRenderTarget.Bind(pd3dImmediateContext);
		g_RGBDRenderer.RenderDepthMap(pd3dImmediateContext, g_CudaDepthSensor.getDepthMapColorSpaceFloat(), g_CudaDepthSensor.getColorMapFilteredFloat4(), g_RGBDAdapter.getWidth(), g_RGBDAdapter.getHeight(), g_RGBDAdapter.getColorIntrinsicsInv(), view, renderIntrinsics, g_CustomRenderTarget.getWidth(), g_CustomRenderTarget.getHeight(), GlobalAppState::get().s_renderingDepthDiscontinuityThresOffset, GlobalAppState::get().s_renderingDepthDiscontinuityThresLin);
		g_CustomRenderTarget.Unbind(pd3dImmediateContext);

		DX11PhongLighting::render(pd3dImmediateContext, g_CustomRenderTarget.GetSRV(1), g_CustomRenderTarget.GetSRV(2), g_CustomRenderTarget.GetSRV(3), GlobalAppState::get().s_useColorForRendering, g_CustomRenderTarget.getWidth(), g_CustomRenderTarget.getHeight());
		DX11QuadDrawer::RenderQuad(pd3dImmediateContext, DX11PhongLighting::GetColorsSRV(), 1.0f);
	}

	// Stop Timing
	if (GlobalAppState::get().s_timingsDetailledEnabled) { GlobalAppState::get().WaitForGPU(); GlobalAppState::get().s_Timer.stop(); TimingLog::totalTimeRender += GlobalAppState::get().s_Timer.getElapsedTimeMS(); TimingLog::countTimeRender++; }


	TimingLog::printTimings();
	if (g_renderText) RenderText();

#ifdef OBJECT_SENSING
	if (bGotDepth == S_OK) ObjectSensing::getInstance()->processFrame(g_sceneRep);
#endif // OBJECT_SENSING
	DXUT_EndPerfEvent();
}


//--------------------------------------------------------------------------------------
// Entry point to the program. Initializes everything and goes into a message processing 
// loop. Idle time is used to render the scene.
//--------------------------------------------------------------------------------------
//int WINAPI main(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
int main(int argc, char** argv)
{
	// Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

#ifdef OBJECT_SENSING
	ObjectSensing::getInstance()->initQtApp(false);
	ObjectSensing::getInstance()->detach();
#endif // OBJECT_SENSING


	try {
		std::string fileNameDescGlobalApp;
		std::string fileNameDescGlobalTracking;
		if (argc == 3) {
			fileNameDescGlobalApp = std::string(argv[1]);
			fileNameDescGlobalTracking = std::string(argv[2]);
		}
		else {
			std::cout << "usage: DepthSensing [fileNameDescGlobalApp] [fileNameDescGlobalTracking]" << std::endl;
			fileNameDescGlobalApp = "zParametersDefault.txt";
			//fileNameDescGlobalApp = "zParametersTango.txt";
			//fileNameDescGlobalApp = "zParametersManolisScan.txt";
			fileNameDescGlobalTracking = "zParametersTrackingDefault.txt";
		}
		std::cout << VAR_NAME(fileNameDescGlobalApp) << " = " << fileNameDescGlobalApp << std::endl;
		std::cout << VAR_NAME(fileNameDescGlobalTracking) << " = " << fileNameDescGlobalTracking << std::endl;
		std::cout << std::endl;

		//Read the global app state
		ParameterFile parameterFileGlobalApp(fileNameDescGlobalApp);
		GlobalAppState::getInstance().readMembers(parameterFileGlobalApp);
		//GlobalAppState::getInstance().print();

		//Read the global camera tracking state
		ParameterFile parameterFileGlobalTracking(fileNameDescGlobalTracking);
		GlobalCameraTrackingState::getInstance().readMembers(parameterFileGlobalTracking);
		//GlobalCameraTrackingState::getInstance().print();

		// Set DXUT callbacks
		DXUTSetCallbackDeviceChanging(ModifyDeviceSettings);
		DXUTSetCallbackMsgProc(MsgProc);
		DXUTSetCallbackKeyboard(OnKeyboard);
		DXUTSetCallbackFrameMove(OnFrameMove);

		DXUTSetCallbackD3D11DeviceAcceptable(IsD3D11DeviceAcceptable);
		DXUTSetCallbackD3D11DeviceCreated(OnD3D11CreateDevice);
		DXUTSetCallbackD3D11SwapChainResized(OnD3D11ResizedSwapChain);
		DXUTSetCallbackD3D11FrameRender(OnD3D11FrameRender);
		DXUTSetCallbackD3D11SwapChainReleasing(OnD3D11ReleasingSwapChain);
		DXUTSetCallbackD3D11DeviceDestroyed(OnD3D11DestroyDevice);

		InitApp();
		DXUTInit(true, true); // Parse the command line, show msgboxes on error, and an extra cmd line param to force REF for now
		DXUTSetCursorSettings(true, true); // Show the cursor and clip it when in full screen
		DXUTCreateWindow(GlobalAppState::get().s_windowWidth, GlobalAppState::get().s_windowHeight, L"VoxelHashing", false);

		DXUTSetIsInGammaCorrectMode(false);	//gamma fix (for kinect)

		DXUTCreateDevice(D3D_FEATURE_LEVEL_11_0, true, GlobalAppState::get().s_windowWidth, GlobalAppState::get().s_windowHeight);
		DXUTMainLoop(); // Enter into the DXUT render loop

	}
	catch (const std::exception& e)
	{
		MessageBoxA(NULL, e.what(), "Exception caught", MB_ICONERROR);
		exit(EXIT_FAILURE);
	}
	catch (...)
	{
		MessageBoxA(NULL, "UNKNOWN EXCEPTION", "Exception caught", MB_ICONERROR);
		exit(EXIT_FAILURE);
	}

	//this is a bit of a hack due to a bug in std::thread (a static object cannot join if the main thread exists)
	auto* s = getRGBDSensor();
	SAFE_DELETE(s);

	return DXUTGetExitCode();
}
