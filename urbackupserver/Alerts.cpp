#include "Alerts.h"
#include "../Interface/Database.h"
#include "database.h"
#include "../Interface/Server.h"
#include "server_settings.h"
#include "../stringtools.h"
#include "../luaplugin/ILuaInterpreter.h"
#include "Mailer.h"
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "../common/miniz.h"

extern ILuaInterpreter* lua_interpreter;

namespace
{
#include "alert_lua.h"

        std::string get_alert_lua()
        {
                size_t out_len;
                void* cdata = tinfl_decompress_mem_to_heap(alert_lua_z, alert_lua_z_len, &out_len, TINFL_FLAG_PARSE_ZLIB_HEADER $
                if (cdata == NULL)
                {
                        return std::string();
                }

                std::string ret(reinterpret_cast<char*>(cdata), reinterpret_cast<char*>(cdata) + out_len);
                mz_free(cdata);
                return ret;
        }

	struct SScriptParam
	{
		std::string name;
		std::string default_value;
	};

	struct SScript
	{
		std::string code;
		std::vector<SScriptParam> params;
		std::string global;
	};

	SScript prepareScript(IDatabase* db, ILuaInterpreter* lua_interpreter, int script_id)
	{
		db_results res_script = db->Read("SELECT script FROM alert_scripts WHERE id="+convert(script_id));

		if (res_script.empty())
		{
			Server->Log("Cannot find alert script with id " + convert(script_id), LL_ERROR);
			return SScript();
		}

		SScript ret;
		ret.code = res_script[0]["script"];

		if (script_id == 1
			&& FileExists("urbackupserver/alert.lua"))
		{
			ret.code = getFile("urbackupserver/alert.lua");
		}

		if(script_id == 1 
			&& ret.code.empty())
		{
			ret.code = get_alert_lua();
		}

		if (!ret.code.empty())
		{
			ret.code = lua_interpreter->compileScript(ret.code);
		}

		if (ret.code.empty())
		{
			return SScript();
		}

		db_results res_params = db->Read("SELECT name, default_value FROM alert_script_params WHERE script_id="+convert(script_id));

		for (size_t i = 0; i < res_params.size(); ++i)
		{
			SScriptParam param = { res_params[i]["name"], res_params[i]["default_value"] };
			ret.params.push_back(param);
		}

		return ret;
	}

	class MailBridge : public ILuaInterpreter::IEMailFunction
	{
	public:
		// Inherited via IEMailFunction
		virtual bool mail(const std::string & send_to, const std::string & subject, const std::string & message)
		{
			return Mailer::sendMail(send_to, subject, message);
		}
	};
}

void Alerts::operator()()
{
	if (lua_interpreter == NULL)
	{
		return;
	}

	IDatabase* db = Server->getDatabase(Server->getThreadID(), URBACKUPDB_SERVER);

	db->Write("UPDATE clients SET alerts_next_check=NULL");

	IQuery* q_get_alert_clients = db->Prepare("SELECT id, name, file_ok, image_ok, alerts_state, strftime('%s', lastbackup) AS lastbackup, "
		"strftime('%s', lastseen) AS lastseen, strftime('%s', lastbackup_image) AS lastbackup_image, created, os_simple "
		"FROM clients WHERE alerts_next_check IS NULL OR alerts_next_check<=?");
	IQuery* q_update_client = db->Prepare("UPDATE clients SET  file_ok=?, image_ok=?, alerts_next_check=?, alerts_state=? WHERE id=?");

	std::map<int, SScript> alert_scripts;

	ILuaInterpreter::SInterpreterFunctions funcs;
	funcs.mail_func = new MailBridge;

	while (true)
	{
		Server->wait(60*1000);

		q_get_alert_clients->Bind(Server->getTimeMS());
		db_results res = q_get_alert_clients->Read();
		q_get_alert_clients->Reset();

		for (size_t i = 0; i < res.size(); ++i)
		{
			int clientid = watoi(res[i]["id"]);
			ServerSettings server_settings(db, clientid);
			int script_id = server_settings.getSettings()->alert_script;
			std::map<int, SScript>::iterator it = alert_scripts.find(script_id);
			if (it == alert_scripts.end())
			{
				alert_scripts[script_id] = prepareScript(db, lua_interpreter, script_id);
				it = alert_scripts.find(script_id);
			}			

			if (!it->second.code.empty())
			{
				ILuaInterpreter::Param params_raw;
				ILuaInterpreter::Param::params_map& params = *params_raw.u.params;
				params["clientid"] = clientid;
				params["clientname"] = res[i]["name"];
				params["incr_file_interval"] = server_settings.getUpdateFreqFileIncr();
				params["full_file_interval"] = server_settings.getUpdateFreqFileFull();
				params["incr_image_interval"] = server_settings.getUpdateFreqImageIncr();
				params["full_image_interval"] = server_settings.getUpdateFreqImageFull();
				params["no_images"] = server_settings.getSettings()->no_images;
				params["no_file_backups"] = server_settings.getSettings()->no_file_backups;
				params["os_simple"] = res[i]["os_simple"];

				int64 times = Server->getTimeSeconds();
				int64 created = watoi64(res[i]["created"]);
				int64 lastbackup_file = watoi64(res[i]["lastbackup"]);
				int64 lastbackup_image = watoi64(res[i]["lastbackup_image"]);

				params["passed_time_lastseen"] = times - watoi64(res[i]["lastseen"]);
				params["passed_time_lastbackup_file"] = (std::min)(times - lastbackup_file, times - created);
				params["passed_time_lastbackup_image"] = (std::min)(times - lastbackup_image, times - created);
				params["lastbackup_file"] = lastbackup_file;
				params["lastbackup_image"] = lastbackup_image;

				SSettings* settings = server_settings.getSettings();

				if (settings->update_freq_full.find(";") != std::string::npos
					|| settings->update_freq_incr.find(";") != std::string::npos
					|| settings->update_freq_image_full.find(";") != std::string::npos
					|| settings->update_freq_image_incr.find(";") != std::string::npos)
				{
					params["complex_interval"] = true;
				}
				else
				{
					params["complex_interval"] = false;
				}

				bool file_ok = res[i]["file_ok"] == "1";
				params["file_ok"] = file_ok;
				bool image_ok = res[i]["image_ok"] == "1";
				params["image_ok"] = image_ok;

				str_map nondefault_params;
				ParseParamStrHttp(server_settings.getSettings()->alert_params, &nondefault_params);

				for (size_t j = 0; j < it->second.params.size(); ++j)
				{
					SScriptParam& param = it->second.params[j];
					str_map::iterator it_param = nondefault_params.find(param.name);
					if (it_param != nondefault_params.end())
					{
						params[param.name] = it_param->second;
					}
					else
					{
						params[param.name] = param.default_value;
					}
				}

				std::string state = res[i]["alerts_state"];
				int64 ret2;
				int64 ret = lua_interpreter->runScript(it->second.code, params_raw, ret2, state, it->second.global, funcs);
				bool needs_update = false;
				
				if (ret>=0)
				{
					file_ok = !(ret & 1);
					image_ok = !(ret & 2);

					if (file_ok != params["file_ok"].u.b
						|| image_ok != params["image_ok"].u.b)
					{
						needs_update = true;
					}
				}
				else
				{
					Server->Log("Error executing alert script id " + convert(script_id) + ". Return value " + convert(ret) + ".", LL_ERROR);
				}

				int64 next_check;
				if (ret2 >= 0)
				{
					next_check = Server->getTimeMS() + ret2;
					needs_update = true;
				}
				else
				{
					if (!file_ok
						&& !image_ok)
					{
						next_check = Server->getTimeMS() + 1*60*60*1000;
						needs_update = true;
					}
					else
					{
						next_check = Server->getTimeMS();
					}
				}

				if (state != res[i]["alerts_state"])
				{
					needs_update = true;
				}

				if (needs_update)
				{
					q_update_client->Bind(file_ok);
					q_update_client->Bind(image_ok);
					q_update_client->Bind(next_check);
					q_update_client->Bind(state);
					q_update_client->Bind(clientid);
					q_update_client->Write();
					q_update_client->Reset();
				}
			}
		}
	}
}
