#include "ClientSession.h"
#include "../GlobalConfig.h"
#include "ClientData.h"
#include "GameData.h"
#include "rzauthGitVersion.h"

#include <stdlib.h>
#include <string.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include "LogServerClient.h"

#include "AuthClient/Flat/TS_AC_AES_KEY_IV.h"
#include "AuthClient/Flat/TS_AC_RESULT.h"
#include "AuthClient/Flat/TS_AC_SELECT_SERVER.h"
#include "AuthClient/Flat/TS_CA_ACCOUNT.h"
#include "AuthClient/Flat/TS_CA_IMBC_ACCOUNT.h"
#include "AuthClient/Flat/TS_CA_RSA_PUBLIC_KEY.h"
#include "AuthClient/Flat/TS_CA_SELECT_SERVER.h"
#include "AuthClient/Flat/TS_CA_SERVER_LIST.h"
#include "AuthClient/Flat/TS_CA_VERSION.h"
#include "AuthClient/TS_AC_SERVER_LIST.h"
#include "GameClient/TS_SC_RESULT.h"
#include "Packet/PacketEpics.h"

namespace AuthServer {

ClientSession::ClientSession()
    : EncryptedSession<PacketSession>(SessionType::AuthClient, SessionPacketOrigin::Server, EPIC_LATEST),
      useRsaAuth(false),
      isEpic2(false),
      lastLoginServerId(1),
      serverIdxOffset(0),
      clientData(nullptr) {}

ClientSession::~ClientSession() {
	if(clientData)
		ClientData::removeClient(clientData);
}

EventChain<SocketSession> ClientSession::onDisconnected(bool causedByRemote) {
	if(clientData) {
		ClientData::removeClient(clientData);
		clientData = nullptr;
	}

	return PacketSession::onDisconnected(causedByRemote);
}

EventChain<PacketSession> ClientSession::onPacketReceived(const TS_MESSAGE* packet) {
	packet_type_id_t packetType = PacketMetadata::convertPacketIdToTypeId(
	    packet->id, SessionType::AuthClient, SessionPacketOrigin::Client, packetVersion);
	switch(packetType) {
		case TS_CA_VERSION::packetID:
			onVersion(static_cast<const TS_CA_VERSION*>(packet));
			break;

		case TS_CA_RSA_PUBLIC_KEY::packetID:
			onRsaKey(static_cast<const TS_CA_RSA_PUBLIC_KEY*>(packet));
			break;

		case TS_CA_ACCOUNT::packetID:
			onAccount(static_cast<const TS_CA_ACCOUNT*>(packet));
			break;

		case TS_CA_IMBC_ACCOUNT::packetID:
			onImbcAccount(static_cast<const TS_CA_IMBC_ACCOUNT*>(packet));
			break;

		case TS_CA_SERVER_LIST::packetID:
			onServerList(static_cast<const TS_CA_SERVER_LIST*>(packet));
			break;

		case TS_CA_SELECT_SERVER::packetID:
			onSelectServer(static_cast<const TS_CA_SELECT_SERVER*>(packet));
			break;

		case 9999:
			break;

		default:
			log(LL_Debug, "Unknown packet ID: %d, size: %d\n", packet->id, packet->size);
			break;
	}

	return PacketSession::onPacketReceived(packet);
}

void ClientSession::onVersion(const TS_CA_VERSION* packet) {
	if(!memcmp(packet->szVersion, "TEST", 4)) {
		uint32_t totalUserCount = ClientData::getClientCount();
		TS_SC_RESULT result;

		result.value = totalUserCount ^ 0xADADADAD;
		result.result = 0;
		result.request_msg_id = packet->id;
		sendPacket(result, EPIC_LATEST);
	} else if(!memcmp(packet->szVersion, "INFO", 4)) {
		static uint32_t gitVersionSuffix = 0;
		TS_SC_RESULT result;

		if(gitVersionSuffix == 0) {
			std::string shaPart(rzauthVersion + 8, 8);
			gitVersionSuffix = strtoul(shaPart.c_str(), nullptr, 16);
		}

		result.value = gitVersionSuffix ^ 0xADADADAD;
		result.result = 0;
		result.request_msg_id = packet->id;
		sendPacket(result, EPIC_LATEST);
	} else if(!memcmp(packet->szVersion, "200609280", 9) || !memcmp(packet->szVersion, "Creer", 5)) {
		isEpic2 = true;
		packetVersion = EPIC_2;
		log(LL_Debug, "Client is epic 2\n");
	}
}

void ClientSession::onRsaKey(const TS_CA_RSA_PUBLIC_KEY* packet) {
	std::unique_ptr<RSA, void (*)(RSA*)> rsaCipher(nullptr, &RSA_free);
	std::unique_ptr<BIO, int (*)(BIO*)> bio(nullptr, &BIO_free);

	const int expectedKeySize = packet->size - sizeof(TS_CA_RSA_PUBLIC_KEY);

	if(packet->key_size != expectedKeySize) {
		log(LL_Warning,
		    "RSA: key_size is invalid: %d, expected (from msg size): %d\n",
		    packet->key_size,
		    expectedKeySize);
		abortSession();
		return;
	}

	ERR_clear_error();

	bio.reset(BIO_new_mem_buf((void*) packet->key, packet->key_size));
	rsaCipher.reset(PEM_read_bio_RSA_PUBKEY(bio.get(), NULL, NULL, NULL));
	if(!rsaCipher) {
		log(LL_Warning, "RSA: invalid certificate: %s\n", ERR_error_string(ERR_get_error(), nullptr));
		abortSession();
		return;
	}

	std::unique_ptr<TS_AC_AES_KEY_IV, void (*)(TS_MESSAGE*)> aesKeyMessage(
	    TS_MESSAGE_WNA::create<TS_AC_AES_KEY_IV, unsigned char>(RSA_size(rsaCipher.get())), &TS_MESSAGE_WNA::destroy);

	for(int i = 0; i < 32; i++)
		aesKey[i] = rand() & 0xFF;

	int blockSize =
	    RSA_public_encrypt(32, aesKey, aesKeyMessage->rsa_encrypted_data, rsaCipher.get(), RSA_PKCS1_PADDING);
	if(blockSize < 0) {
		log(LL_Warning, "RSA: encrypt error: %s\n", ERR_error_string(ERR_get_error(), nullptr));
		abortSession();
		return;
	}

	aesKeyMessage->data_size = blockSize;

	useRsaAuth = true;
	sendPacket(aesKeyMessage.get());
}

void ClientSession::onAccount(const TS_CA_ACCOUNT* packet) {
	std::string account;
	std::vector<unsigned char> cryptedPassword;

	if(dbQuery.inProgress()) {
		TS_AC_RESULT result;
		TS_MESSAGE::initMessage<TS_AC_RESULT>(&result);
		result.request_msg_id = TS_CA_ACCOUNT::packetID;
		result.result = TS_RESULT_CLIENT_SIDE_ERROR;
		result.login_flag = 0;
		sendPacket(&result);
		log(LL_Info, "Client connection with a auth request already in progress\n");
		return;
	}

	if(useRsaAuth) {
		const TS_CA_ACCOUNT_RSA* accountv2 = reinterpret_cast<const TS_CA_ACCOUNT_RSA*>(packet);

		account = Utils::convertToString(accountv2->account, sizeof(accountv2->account) - 1);
		cryptedPassword =
		    Utils::convertToDataArray(accountv2->password, sizeof(accountv2->password), accountv2->password_size);
	} else {
		if(packet->size == sizeof(TS_CA_ACCOUNT_EPIC4)) {
			const TS_CA_ACCOUNT_EPIC4* accountE4 = reinterpret_cast<const TS_CA_ACCOUNT_EPIC4*>(packet);

			// If not already logged, log client epic <= 4
			if(!isEpic2)
				log(LL_Debug, "Client is epic 4 or older\n");

			account = Utils::convertToString(accountE4->account, sizeof(accountE4->account) - 1);
			cryptedPassword = Utils::convertToDataArray(accountE4->password, sizeof(accountE4->password));
		} else {
			account = Utils::convertToString(packet->account, sizeof(packet->account) - 1);
			cryptedPassword = Utils::convertToDataArray(packet->password, sizeof(packet->password));
		}
	}

	log(LL_Debug, "Login request for account %s\n", account.c_str());

	DB_AccountData::Input input(account,
	                            getStream()->getRemoteAddress(),
	                            useRsaAuth ? DB_AccountData::EM_AES : DB_AccountData::EM_DES,
	                            cryptedPassword,
	                            aesKey);
	dbQuery.executeDbQuery<DB_AccountData, DB_Account>(this, &ClientSession::clientAuthResult, input);
}

void ClientSession::onImbcAccount(const TS_CA_IMBC_ACCOUNT* packet) {
	std::string account;
	std::vector<unsigned char> cryptedPassword;

	if(dbQuery.inProgress()) {
		TS_AC_RESULT result;
		TS_MESSAGE::initMessage<TS_AC_RESULT>(&result);
		result.request_msg_id = TS_CA_ACCOUNT::packetID;
		result.result = TS_RESULT_CLIENT_SIDE_ERROR;
		result.login_flag = 0;
		sendPacket(&result);
		log(LL_Info, "Client IMBC connection with a auth request already in progress\n");
		return;
	}

	if(useRsaAuth) {
		const TS_CA_IMBC_ACCOUNT_RSA* accountv2 = reinterpret_cast<const TS_CA_IMBC_ACCOUNT_RSA*>(packet);

		account = Utils::convertToString(accountv2->account, sizeof(accountv2->account) - 1);
		cryptedPassword =
		    Utils::convertToDataArray(accountv2->password, sizeof(accountv2->password), accountv2->password_size);
	} else if(packet->size == sizeof(TS_CA_IMBC_ACCOUNT_OLD)) {
		const TS_CA_IMBC_ACCOUNT_OLD* accountOld = reinterpret_cast<const TS_CA_IMBC_ACCOUNT_OLD*>(packet);
		account = Utils::convertToString(accountOld->account, sizeof(accountOld->account) - 1);
		cryptedPassword = Utils::convertToDataArray(accountOld->password, sizeof(accountOld->password));
	} else {
		account = Utils::convertToString(packet->account, sizeof(packet->account) - 1);
		cryptedPassword = Utils::convertToDataArray(packet->password, sizeof(packet->password));
	}

	if(CONFIG_GET()->auth.client.enableImbc.get() == false) {
		TS_AC_RESULT result;
		TS_MESSAGE::initMessage<TS_AC_RESULT>(&result);
		result.request_msg_id = TS_CA_ACCOUNT::packetID;
		result.result = TS_RESULT_ACCESS_DENIED;
		result.login_flag = 0;
		sendPacket(&result);
		log(LL_Debug, "Refused IMBC connection (IMBC is disabled) for account %s\n", account.c_str());
	} else {
		log(LL_Debug, "IMBC Login request for account %s\n", account.c_str());

		DB_AccountData::Input input(account,
		                            getStream()->getRemoteAddress(),
		                            useRsaAuth ? DB_AccountData::EM_AES : DB_AccountData::EM_None,
		                            cryptedPassword,
		                            aesKey);
		dbQuery.executeDbQuery<DB_AccountData, DB_Account>(this, &ClientSession::clientAuthResult, input);
	}
}

void ClientSession::clientAuthResult(DB_Account* query) {
	TS_AC_RESULT result;
	TS_MESSAGE::initMessage<TS_AC_RESULT>(&result);

	result.request_msg_id = TS_CA_ACCOUNT::packetID;
	result.login_flag = 0;

	auto& results = query->getResults();
	if(results.size() == 0 || results.size() > 1) {
		result.result = TS_RESULT_NOT_EXIST;
		sendPacket(&result);
		return;
	}

	const DB_AccountData::Output* output = results.front().get();
	const DB_AccountData::Input* input = query->getInput();

	if(output->ok == false || output->auth_ok == false) {
		result.result = TS_RESULT_NOT_EXIST;
		result.login_flag = 0;
	} else if(output->block == true) {
		result.result = TS_RESULT_ACCESS_DENIED;
		result.login_flag = 0;
	} else if(clientData != nullptr) {  // already connected
		result.result = TS_RESULT_CLIENT_SIDE_ERROR;
		result.login_flag = 0;
		log(LL_Info, "Client connection already authenticated with account %s\n", clientData->account.c_str());
	} else {
		ClientData* oldClient;
		char ip[INET6_ADDRSTRLEN];

		getStream()->getRemoteAddress().getName(ip, sizeof(ip));

		clientData = ClientData::tryAddClient(
		    this, input->account, output->account_id, output->age, output->event_code, output->pcbang, ip, &oldClient);
		if(clientData == nullptr) {
			result.result = TS_RESULT_ALREADY_EXIST;
			result.login_flag = 0;
			log(LL_Info, "Client %s already connected\n", input->account.c_str());

			GameData* oldCientGameData = oldClient->getGameServer();

			if(!oldCientGameData) {
				LogServerClient::sendLog(LogServerClient::LM_ACCOUNT_DUPLICATE_AUTH_LOGIN,
				                         output->account_id,
				                         0,
				                         0,
				                         0,
				                         0,
				                         0,
				                         0,
				                         0,
				                         0,
				                         0,
				                         0,
				                         input->account.c_str(),
				                         -1,
				                         ip,
				                         -1,
				                         oldClient->ip,
				                         -1,
				                         0,
				                         0);

				oldClient->getClientSession()->abortSession();
			} else {
				LogServerClient::sendLog(LogServerClient::LM_ACCOUNT_DUPLICATE_GAME_LOGIN,
				                         output->account_id,
				                         0,
				                         0,
				                         oldCientGameData->getServerIdx(),
				                         0,
				                         0,
				                         0,
				                         0,
				                         0,
				                         0,
				                         0,
				                         input->account.c_str(),
				                         -1,
				                         ip,
				                         -1,
				                         oldClient->ip,
				                         -1,
				                         0,
				                         0);

				if(oldClient->isConnectedToGame())
					oldCientGameData->kickClient(oldClient);
				else {
					ClientData::removeClient(oldClient);
				}
			}
		} else {
			result.result = 0;
			result.login_flag = TS_AC_RESULT::LSF_EULA_ACCEPTED;
			this->lastLoginServerId = output->last_login_server_idx;
			this->serverIdxOffset = output->server_idx_offset;
		}
	}

	sendPacket(&result);
}

void ClientSession::onServerList(const TS_CA_SERVER_LIST* packet) {
	TS_AC_SERVER_LIST serverListPacket;

	int maxPublicServerBaseIdx = CONFIG_GET()->auth.client.maxPublicServerIdx;

	// Check if user authenticated
	if(clientData == nullptr) {
		abortSession();
		return;
	}

	const std::unordered_map<uint16_t, GameData*>& serverList = GameData::getServerList();
	std::unordered_map<uint16_t, GameData*>::const_iterator it, itEnd;

	unsigned int maxPlayers = CONFIG_GET()->auth.game.maxPlayers;

	serverListPacket.servers.reserve(serverList.size());
	serverListPacket.last_login_server_idx = lastLoginServerId;

	for(it = serverList.cbegin(), itEnd = serverList.cend(); it != itEnd; ++it) {
		GameData* serverInfo = it->second;

		// servers with their index higher than maxPublicServerBaseIdx + serverIdxOffset are hidden
		// serverIdxOffset is a per user value from the DB, default to 0
		// maxPublicServerBaseIdx is a config value, default to 30
		// So by default, servers with index > 30 are not shown in client's server list
		if(serverInfo->getServerIdx() > maxPublicServerBaseIdx + serverIdxOffset)
			continue;

		// Don't display not ready game servers (offline or not yet received all player list)
		if(!serverInfo->isReady())
			continue;

		serverListPacket.servers.push_back(TS_SERVER_INFO());
		TS_SERVER_INFO& serverData = serverListPacket.servers.back();

		serverData.server_idx = serverInfo->getServerIdx();
		serverData.server_port = serverInfo->getServerPort();
		serverData.is_adult_server = serverInfo->getIsAdultServer();
		serverData.server_ip = serverInfo->getServerIp();
		serverData.server_name = serverInfo->getServerName();
		serverData.server_screenshot_url = serverInfo->getServerScreenshotUrl();

		uint32_t userRatio = serverInfo->getPlayerCount() * 100 / maxPlayers;
		serverData.user_ratio = (userRatio > 100) ? 100 : userRatio;
	}

	sendPacket(serverListPacket, isEpic2 ? EPIC_2 : EPIC_9_1);
}

void ClientSession::onSelectServer(const TS_CA_SELECT_SERVER* packet) {
	const std::unordered_map<uint16_t, GameData*>& serverList = GameData::getServerList();

	if(clientData == nullptr) {
		abortSession();
		return;
	}

	if(serverList.find(packet->server_idx) != serverList.end()) {
		GameData* server = serverList.at(packet->server_idx);
		uint64_t oneTimePassword = ((uint64_t) rand()) * rand() * rand() * rand();

		DbQueryJob<DB_UpdateLastServerIdx>::executeNoResult(
		    DB_UpdateLastServerIdx::Input(clientData->accountId, packet->server_idx));

		// clientData now managed by target GS
		clientData->switchClientToServer(server, oneTimePassword);
		clientData = nullptr;

		log(LL_Debug, "Client choose server idx %d\n", packet->server_idx);

		if(useRsaAuth) {
			TS_AC_SELECT_SERVER_RSA result;
			TS_MESSAGE::initMessage<TS_AC_SELECT_SERVER_RSA>(&result);
			result.result = 0;
			result.encrypted_data_size = 16;
			result.pending_time = 0;
			result.unknown = 0;
			result.unknown2 = 0;

			std::unique_ptr<EVP_CIPHER_CTX, void (*)(EVP_CIPHER_CTX*)> e_ctx(nullptr, &EVP_CIPHER_CTX_free);
			int bytesWritten;
			bool ok = false;

			e_ctx.reset(EVP_CIPHER_CTX_new());
			if(!e_ctx)
				goto cleanup;

			if(EVP_EncryptInit_ex(e_ctx.get(), EVP_aes_128_cbc(), NULL, aesKey, aesKey + 16) < 0)
				goto cleanup;
			if(EVP_EncryptUpdate(e_ctx.get(),
			                     result.encrypted_data,
			                     &bytesWritten,
			                     (const unsigned char*) &oneTimePassword,
			                     sizeof(uint64_t)) < 0)
				goto cleanup;
			if(EVP_EncryptFinal_ex(e_ctx.get(), result.encrypted_data + bytesWritten, &bytesWritten) < 0)
				goto cleanup;

			sendPacket(&result);
			ok = true;

		cleanup:
			if(!ok)
				abortSession();
		} else {
			TS_AC_SELECT_SERVER result;
			TS_MESSAGE::initMessage<TS_AC_SELECT_SERVER>(&result);

			result.result = 0;
			result.one_time_key = oneTimePassword;
			result.pending_time = 0;

			sendPacket(&result);
		}
	} else {
		abortSession();
		log(LL_Warning, "Attempt to connect to an invalid server idx: %d\n", packet->server_idx);
	}
}

}  // namespace AuthServer
