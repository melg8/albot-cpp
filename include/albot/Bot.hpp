#ifndef ALBOT_BOT_HPP_
#define ALBOT_BOT_HPP_

#include "albot/GameInfo.hpp"

#define PROXY_GETTER(capName, type) type get##capName();

enum CharacterType { PRIEST = 1, MAGE = 2, RANGER = 3, ROGUE = 4, WARRIOR = 5, PALADIN = 6, MERCHANT = 7 };

class Bot {
	protected:
		std::shared_ptr<spdlog::logger> mLogger;
	public:
		Bot(const CharacterGameInfo& id);
		nlohmann::json data;
		nlohmann::json party;
		const CharacterGameInfo& info;
		std::string name;
		size_t id;
		void log(std::string str);
		void join_server(std::string str);
		void login();
		bool isMoving();
		bool isAlive();
	    virtual void onPartyRequest(std::string /* name */) {};
	    virtual void onPartyInvite(std::string /* name */) {};
		virtual void onCm(const std::string& /* name */, const nlohmann::json& /* data */) {};
		virtual void onPm(const std::string& /* name */, const std::string& /* message */) {};
		virtual void onChat(const std::string& /* name */, const std::string& /* message */) {};
		virtual void onConnect();
		virtual ~Bot() {};
		virtual void start() {};
		virtual void stop() {};
		void updateJson(const nlohmann::json&);
		std::string getUsername();
		nlohmann::json& getRawJson();
		void setParty(const nlohmann::json& j);

		PROXY_GETTER(X, double)
		PROXY_GETTER(Y, double)
		PROXY_GETTER(Hp, int)
		PROXY_GETTER(MaxHp, int)
		PROXY_GETTER(Mp, int)
		PROXY_GETTER(MaxMp, int)
		PROXY_GETTER(Map, std::string)
		PROXY_GETTER(MapId, int)
		PROXY_GETTER(Range, int)
		PROXY_GETTER(CType, std::string)
		PROXY_GETTER(Speed, int)
		PROXY_GETTER(Gold, long long)
		PROXY_GETTER(Id, std::string)
			
};

#endif /* ALBOT_BOT_HPP_ */