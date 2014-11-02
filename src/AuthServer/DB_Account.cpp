#include "DB_Account.h"
#include <openssl/md5.h>
#include "ClientSession.h"
#include "../GlobalConfig.h"

#ifdef _MSC_VER
#define strncasecmp strnicmp
#endif


template<> DbQueryBinding* DbQueryJob<AuthServer::DB_Account>::dbBinding = nullptr;

namespace AuthServer {

struct DbAccountConfig {
	cval<bool>& enable;
	cval<std::string>& query;
	cval<int> &paramAccount, &paramPassword;
	cval<std::string> &colAccountId, &colPassword, &colAuthOk, &colAge, &colLastServerIdx, &colEventCode, &colPcBang, &colServerIdxOffset, &colBlock;

	DbAccountConfig() :
		enable(CFG_CREATE("sql.db_account.enable", true)),
		query(CFG_CREATE("sql.db_account.query", "SELECT * FROM account WHERE account = ? AND password = ?;")),
		paramAccount (CFG_CREATE("sql.db_account.param.account" , 1)),
		paramPassword(CFG_CREATE("sql.db_account.param.password", 2)),
		colAccountId    (CFG_CREATE("sql.db_account.column.accountid"    , "account_id")),
		colPassword     (CFG_CREATE("sql.db_account.column.password"    , "password")),
		colAuthOk       (CFG_CREATE("sql.db_account.column.authok"    , "auth_ok")),
		colAge          (CFG_CREATE("sql.db_account.column.age"          , "age")),
		colLastServerIdx(CFG_CREATE("sql.db_account.column.lastserveridx", "last_login_server_idx")),
		colEventCode    (CFG_CREATE("sql.db_account.column.eventcode"    , "event_code")),
		colPcBang    (CFG_CREATE("sql.db_account.column.pcbang"    , "pcbang")),
		colServerIdxOffset    (CFG_CREATE("sql.db_account.column.serveridxoffset", "server_idx_offset")),
		colBlock(CFG_CREATE("sql.db_account.column.block", "block"))
	{}
};
static DbAccountConfig* config = nullptr;

bool DB_Account::init(DbConnectionPool* dbConnectionPool) {
	std::vector<DbQueryBinding::ParameterBinding> params;
	std::vector<DbQueryBinding::ColumnBinding> cols;

	config = new DbAccountConfig;

	params.emplace_back(DECLARE_PARAMETER(DB_Account, account, 0, config->paramAccount));
	params.emplace_back(DECLARE_PARAMETER(DB_Account, givenPasswordString, 32, config->paramPassword));

	cols.emplace_back(DECLARE_COLUMN(DB_Account, accountId, 0, config->colAccountId));
	cols.emplace_back(DECLARE_COLUMN_WITH_INFO(DB_Account, password, sizeof(((DB_Account*)(0))->password), config->colPassword, nullPassword));
	cols.emplace_back(DECLARE_COLUMN(DB_Account, authOk, 0, config->colAuthOk));
	cols.emplace_back(DECLARE_COLUMN(DB_Account, age, 0, config->colAge));
	cols.emplace_back(DECLARE_COLUMN(DB_Account, lastServerIdx, 0, config->colLastServerIdx));
	cols.emplace_back(DECLARE_COLUMN(DB_Account, eventCode, 0, config->colEventCode));
	cols.emplace_back(DECLARE_COLUMN(DB_Account, pcBang, 0, config->colPcBang));
	cols.emplace_back(DECLARE_COLUMN(DB_Account, serverIdxOffset, 0, config->colServerIdxOffset));
	cols.emplace_back(DECLARE_COLUMN(DB_Account, block, 0, config->colBlock));

	dbBinding = new DbQueryBinding(dbConnectionPool, config->enable, CONFIG_GET()->auth.db.connectionString, config->query, params, cols);

	return true;
}

void DB_Account::deinit() {
	DbQueryBinding* binding = dbBinding;
	dbBinding = nullptr;
	delete binding;
	delete config;
}

DB_Account::DB_Account(ClientSession* clientInfo, const std::string& account, const char* password, size_t size)
	: clientInfo(clientInfo), account(account)
{
	std::string buffer = CONFIG_GET()->auth.db.salt;
	ok = false;

	accountId = 0xFFFFFFFF;
	this->password[0] = '\0';
	nullPassword = true;
	authOk = true;
	age = 19;
	lastServerIdx = 1;
	eventCode = 0;
	pcBang = 0;
	serverIdxOffset = 0;
	block = false;

	buffer.append(password, password + size);
	trace("MD5 of \"%.*s\" with len %d\n", (int)buffer.size(), buffer.c_str(), (int)buffer.size());
	MD5((const unsigned char*)buffer.c_str(), buffer.size(), givenPasswordMd5);

	execute(DbQueryBinding::EM_OneRow);
}

bool DB_Account::onPreProcess() {
	//Accounts with @ before the name are banned accounts
	if(account.size() == 0 || account[0] == '@') {
		debug("Account name has invalid character at start: %s\n", account.c_str());
		return false;
	}

	for(int i = 0; i < 16; i++) {
		unsigned char val = givenPasswordMd5[i] >> 4;
		if(val < 10)
			givenPasswordString[i*2] = val + '0';
		else
			givenPasswordString[i*2] = val - 10 + 'a';

		val = givenPasswordMd5[i] & 0x0F;
		if(val < 10)
			givenPasswordString[i*2+1] = val + '0';
		else
			givenPasswordString[i*2+1] = val - 10 + 'a';
	}
	givenPasswordString[32] = '\0';

	trace("Querying for account \"%s\" and password MD5 \"%s\"\n", account.c_str(), givenPasswordString);

	return true;
}

bool DB_Account::onRowDone() {
	if(accountId == 0xFFFFFFFF)
		return false;

	// Si pas de colonne password, le mdp est checké
	if(nullPassword == true && password[0] == '\0') {
		ok = true;
	} else if(!strcmp(givenPasswordString, password)){
		ok = true;
	}

	return false;
}

void DB_Account::onDone(Status status) {
	if(status != S_Canceled && clientInfo)
		clientInfo->clientAuthResult(ok && authOk, account, accountId, age, lastServerIdx, eventCode, pcBang, serverIdxOffset, block);
}

} // namespace AuthServer
