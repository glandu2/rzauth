#pragma once

#include "ClientData.h"
#include "DB_SecurityNoCheck.h"
#include "NetSession/PacketSession.h"
#include <array>
#include <list>
#include <stdint.h>
#include <string>
#include <unordered_map>

#include "AuthGame/TS_GA_ACCOUNT_LIST.h"
#include "AuthGame/TS_GA_CLIENT_KICK_FAILED.h"
#include "AuthGame/TS_GA_CLIENT_LOGIN.h"
#include "AuthGame/TS_GA_CLIENT_LOGOUT.h"
#include "AuthGame/TS_GA_LOGIN.h"
#include "AuthGame/TS_GA_LOGOUT.h"
#include "AuthGame/TS_GA_SECURITY_NO_CHECK.h"
#include "PacketEnums.h"

struct TS_AG_CLIENT_LOGIN;
struct TS_AG_CLIENT_LOGIN_EXTENDED;

namespace AuthServer {

class GameData;
class DB_SecurityNoCheck;

class GameServerSession : public PacketSession {
	DECLARE_CLASS(AuthServer::GameServerSession)

public:
	GameServerSession();

	void kickClient(ClientData* clientData);
	void sendNotifyItemPurchased(ClientData* client);
	void sendNotifyItemSupplied(ClientData* client);

	void setGameData(GameData* gameData);

	void onSecurityNoCheckResult(DB_SecurityNoCheck* query);

protected:
	EventChain<SocketSession> onConnected();
	EventChain<PacketSession> onPacketReceived(const TS_MESSAGE* packet);

	void onServerLogin(const TS_GA_LOGIN* packet, const std::array<uint8_t, 16>* guid);
	void onAccountList(const TS_GA_ACCOUNT_LIST* packet);
	void onServerLogout(const TS_GA_LOGOUT* packet);
	void onClientLogin(const TS_GA_CLIENT_LOGIN* packet);
	void onClientLogout(const TS_GA_CLIENT_LOGOUT* packet);
	void onClientKickFailed(const TS_GA_CLIENT_KICK_FAILED* packet);
	void onSecurityNoCheck(const TS_GA_SECURITY_NO_CHECK* packet);

private:
	void fillClientLoginResult(TS_AG_CLIENT_LOGIN* packet,
	                           const char* account,
	                           TS_ResultCode result,
	                           ClientData* clientData);
	void fillClientLoginExtendedResult(TS_AG_CLIENT_LOGIN_EXTENDED* packet,
	                                   const char* account,
	                                   TS_ResultCode result,
	                                   ClientData* clientData);
	void sendClientLoginResult(const char* account, TS_ResultCode result, ClientData* clientData);

private:
	~GameServerSession();

	GameData* gameData;
	bool useAutoReconnectFeature;
	bool securityNoSendMode;  // if true, send mode in security no reply (with e6+)

	std::vector<TS_GA_ACCOUNT_LIST::AccountInfo> alreadyConnectedAccounts;
	DbQueryJobRef securityNoCheckQueries;
};

}  // namespace AuthServer

