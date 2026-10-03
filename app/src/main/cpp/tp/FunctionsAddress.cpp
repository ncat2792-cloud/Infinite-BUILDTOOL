#include "FunctionsAddress.h"

/*Class : PythonUtils*/
// NetEase x19 3.9.5.297103 (low) -> 3.9.15.297907 (high) address table.
// Python-region code (PyImport/PyModule/PyRun/PyGILState) shifted by +0x1A90;
// setExecLocked (.bss bool) moved 0x1351D350 -> 0x1351EE70, verified through
// its paired set/get leaf accessors which share the +0x1A90 code shift.
uintptr_t FunctionsAddress::PythonUtils_setExecLocked                  = 0x1351EE70; //U
uintptr_t FunctionsAddress::PythonUtils_PyGILState_EnsureMC            = 0x12356D88; //U
uintptr_t FunctionsAddress::PythonUtils_PyImport_AddModuleMC           = 0x122D63F0; //U
uintptr_t FunctionsAddress::PythonUtils_PyModule_GetDictMC             = 0x1224ABA0; //U
uintptr_t FunctionsAddress::PythonUtils_PyRun_StringFlagsMC            = 0x123586A4; //U
uintptr_t FunctionsAddress::PythonUtils_PyGILState_ReleaseMC           = 0x12356E20; //U


/*Actor::normalTick - 游戏主循环 hook*/
uintptr_t FunctionsAddress::Minecraft_update_Hook = 0x74C79FC;

uintptr_t FunctionsAddress::Actor_getClientInstance = 0x74C673C;
uintptr_t FunctionsAddress::Actor_getDimension = 0xD8E7554;

/*Level::_render - 世界渲染完成回调*/
uintptr_t FunctionsAddress::Level_Render_Hook = 0x7B73A54;

/*ClientInstance::getCamera - 投影矩阵访问*/
uintptr_t FunctionsAddress::ClientInstance_getCamera = 0x7463BF4;

/*newReceivePacket - 网络包接收*/
uintptr_t FunctionsAddress::newReceivePacket_Hook = 0xA7905A4;
