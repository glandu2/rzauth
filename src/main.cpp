#include "EventLoop.h"
#include "GlobalConfig.h"
#include "RappelzLibInit.h"
#include "RappelzLibConfig.h"
#include "CrashHandler.h"

#include "AuthServer/DB_Account.h"
#include "ServersManager.h"
#include "BanManager.h"
#include "SocketSession.h"

#include "AuthServer/ClientSession.h"
#include "AuthServer/GameServerSession.h"
#include "AuthServer/DB_Account.h"

#include "UploadServer/ClientSession.h"
#include "UploadServer/GameServerSession.h"
#include "UploadServer/IconServerSession.h"

#include "AdminServer/TelnetSession.h"


/* TODO
 * Log packets
 * Telnet for commands (like stop which would help to have a correct valgrind output):
 *  - stats - show stats of the server (player count, active connections, connected GS, ...)
 *  - dump - dump variables
 */

void runServers(Log* trafficLogger);
void showDebug(uv_timer_t*);

int main(int argc, char **argv) {
	CrashHandler::setProcessExceptionHandlers();
	CrashHandler::setThreadExceptionHandlers();

	RappelzLibInit(argc, argv, nullptr);

	GlobalConfig::init();
	//Only set CONFIG_FILE_KEY
	ConfigInfo::get()->parseCommandLine(argc, argv, true);
	ConfigInfo::get()->readFile(RappelzLibConfig::get()->app.configfile.get().c_str());
	//Set all keys given on the command line to overwrite config file values
	ConfigInfo::get()->parseCommandLine(argc, argv);

	Log mainLogger(RappelzLibConfig::get()->log.enable,
					RappelzLibConfig::get()->log.level,
					RappelzLibConfig::get()->log.consoleLevel,
					RappelzLibConfig::get()->log.dir,
					RappelzLibConfig::get()->log.file);
	Log::setDefaultLogger(&mainLogger);

	Log trafficLogger(CONFIG_GET()->trafficDump.enable,
					CONFIG_GET()->trafficDump.level,
					CONFIG_GET()->trafficDump.consoleLevel,
					CONFIG_GET()->trafficDump.dir,
					CONFIG_GET()->trafficDump.file);

	ConfigInfo::get()->dump();

	if(AuthServer::DB_Account::init() == false) {
		return -1;
	}

	CrashHandler::setDumpMode(CONFIG_GET()->admin.dumpMode);

	runServers(&trafficLogger);

	//Make valgrind happy
	EventLoop::getInstance()->deleteObjects();

	return 0;
}

void runServers(Log *trafficLogger) {
	ServersManager serverManager;
	BanManager banManager;

	RappelzServer<AuthServer::ClientSession> authClientServer(trafficLogger);
	RappelzServer<AuthServer::GameServerSession> authGameServer(trafficLogger);

	RappelzServer<UploadServer::ClientSession> uploadClientServer(trafficLogger);
	RappelzServer<UploadServer::IconServerSession> uploadIconServer;
	RappelzServer<UploadServer::GameServerSession> uploadGameServer(trafficLogger);

	RappelzServer<AdminServer::TelnetSession> adminTelnetServer;

	banManager.loadFile();


	serverManager.addServer("auth.clients", &authClientServer,
							CONFIG_GET()->auth.client.listenIp,
							CONFIG_GET()->auth.client.port,
							CONFIG_GET()->auth.client.autoStart,
							&banManager);
	serverManager.addServer("auth.gameserver", &authGameServer,
							CONFIG_GET()->auth.game.listenIp,
							CONFIG_GET()->auth.game.port,
							CONFIG_GET()->auth.game.autoStart);

	serverManager.addServer("upload.clients", &uploadClientServer,
							CONFIG_GET()->upload.client.listenIp,
							CONFIG_GET()->upload.client.port,
							CONFIG_GET()->upload.client.autoStart,
							&banManager);
	serverManager.addServer("upload.iconserver", &uploadIconServer,
							CONFIG_GET()->upload.client.listenIp,
							CONFIG_GET()->upload.client.webPort,
							CONFIG_GET()->upload.client.autoStart,
							&banManager);
	serverManager.addServer("upload.gameserver", &uploadGameServer,
							CONFIG_GET()->upload.game.listenIp,
							CONFIG_GET()->upload.game.port,
							CONFIG_GET()->upload.game.autoStart);

	serverManager.addServer("admin.telnet", &adminTelnetServer,
							CONFIG_GET()->admin.telnet.listenIp,
							CONFIG_GET()->admin.telnet.port,
							CONFIG_GET()->admin.telnet.autoStart);

	serverManager.start();

	EventLoop::getInstance()->run(UV_RUN_DEFAULT);
}

void showDebug(uv_timer_t *) {
	char debugInfo[1000];
	strcpy(debugInfo, "----------------------------------\n");
	sprintf(debugInfo, "%s%lu socket Sessions\n", debugInfo, SocketSession::getObjectCount());
	sprintf(debugInfo, "%s%lu active connections\n", debugInfo, Socket::getObjectCount());
	puts(debugInfo);
}
