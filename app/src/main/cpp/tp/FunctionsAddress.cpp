#include "FunctionsAddress.h"

/* Class: PythonUtils - retired.
 *
 * The 3.9.5 client executed embedded Python through these fixed RVAs. The
 * 3.10.5 client no longer exports that entry point, so the values below are
 * deliberately cleared: keeping stale addresses here would let a caller jump
 * into arbitrary code after a client update. Python execution now goes through
 * pyengine/PyEngineBridge, which resolves the 3.10.5 API by genuine code
 * signatures and refuses to run when verification fails.
 */
uintptr_t FunctionsAddress::PythonUtils_setExecLocked                  = 0;
uintptr_t FunctionsAddress::PythonUtils_PyGILState_EnsureMC            = 0;
uintptr_t FunctionsAddress::PythonUtils_PyImport_AddModuleMC           = 0;
uintptr_t FunctionsAddress::PythonUtils_PyModule_GetDictMC             = 0;
uintptr_t FunctionsAddress::PythonUtils_PyRun_StringFlagsMC            = 0;
uintptr_t FunctionsAddress::PythonUtils_PyGILState_ReleaseMC           = 0;


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
