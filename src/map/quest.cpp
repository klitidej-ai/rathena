// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "quest.hpp"

#include <stdlib.h>

#include "../common/cbasetypes.hpp"
#include "../common/malloc.hpp"
#include "../common/nullpo.hpp"
#include "../common/random.hpp"
#include "../common/showmsg.hpp"
#include "../common/socket.hpp"
#include "../common/strlib.hpp"
#include "../common/utilities.hpp"

#include "battle.hpp"
#include "chrif.hpp"
#include "clif.hpp"
#include "intif.hpp"
#include "itemdb.hpp"
#include "log.hpp"
#include "map.hpp"
#include "mob.hpp"
#include "party.hpp"
#include "pc.hpp"

const std::string QuestDatabase::getDefaultLocation() {
	return std::string(db_path) + "/quest_db.yml";
}

/**
 * Reads and parses an entry from the quest_db.
 * @param node: YAML node containing the entry.
 * @return count of successfully parsed rows
 */
uint64 QuestDatabase::parseBodyNode(const YAML::Node &node) {
	uint32 quest_id;

	if (!this->asUInt32(node, "Id", quest_id))
		return 0;

	std::shared_ptr<s_quest_db> quest = this->find(quest_id);
	bool exists = quest != nullptr;

	if (this->nodeExists(node, "TimeLimit") && (this->nodeExists(node, "TimeInDay") || this->nodeExists(node, "TimeAtHour") || this->nodeExists(node, "TimeAtMinute"))) {
		this->invalidWarning(node, "Node \"TimeLimit\" cannot be defined with \"TimeInDay\", \"TimeAtHour\", or \"TimeAtMinute\".\n");
		return 0;
	}

	if (!exists) {
		if (!this->nodeExists(node, "Title")) {
			this->invalidWarning(node, "Node \"Title\" is missing.\n");
			return 0;
		}

		quest = std::make_shared<s_quest_db>();
		quest->id = quest_id;
	}

	if (this->nodeExists(node, "Title")) {
		std::string name;

		if (!this->asString(node, "Title", name)) {
			return 0;
		}

		quest->name = name;
	}

	if (this->nodeExists(node, "TimeLimit")) {
		uint32 time;

		if (!this->asUInt32(node, "TimeLimit", time))
			return 0;

		quest->time = time;

		quest->timeday = 0;
		quest->timehour = -1;
		quest->timeminute = -1;
	} else if (this->nodeExists(node, "TimeInDay") || this->nodeExists(node, "TimeAtHour") || this->nodeExists(node, "TimeAtMinute")) {

		if (!exists) {
			if (!this->nodeExists(node, "TimeAtMinute")) {
				this->invalidWarning(node, "Node \"TimeAtMinute\" is missing.\n");
				return 0;
			}
		} else {
			if (quest->timeminute < 0 && !this->nodeExists(node, "TimeAtMinute")) {
				this->invalidWarning(node, "Node \"TimeAtMinute\" is missing.\n");
				return 0;
			}
		}

		if (this->nodeExists(node, "TimeInDay")) {
			uint16 time;

			if (!this->asUInt16(node, "TimeInDay", time))
				return 0;

			quest->timeday = time;
		} else {
			if (!exists)
				quest->timeday = 0;
		}

		if (this->nodeExists(node, "TimeAtHour")) {
			int16 time;

			if (!this->asInt16(node, "TimeAtHour", time))
				return 0;

			if (time > 23) {
				this->invalidWarning(node, "TimeAtHour %hu exceeds 23 hours. Capping to 23.\n", time);
				time = 23;
			}

			quest->timehour = time;
		} else {
			if (!exists) 
				quest->timehour = -1;
		}
		
		if (this->nodeExists(node, "TimeAtMinute")) {
			int16 time;

			if (!this->asInt16(node, "TimeAtMinute", time))
				return 0;

			if (time > 59) {
				this->invalidWarning(node, "TimeAtMinute %hu exceeds 59 minutes. Capping to 59.\n", time);
				time = 59;
			}
			else if (time < 0)
				time = 0;

			quest->timeminute = time;
		}

		quest->time = 0;
	} else {
		if (!exists) {
			quest->time = 0;

			quest->timeday = 0;
			quest->timehour = -1;
			quest->timeminute = -1;
		}
	}

	if (this->nodeExists(node, "Target")) {
		const YAML::Node &targets = node["Target"];

		for (const YAML::Node &targetNode : targets) {
			if (quest->objectives.size() >= MAX_QUEST_OBJECTIVES) {
				this->invalidWarning(targetNode, "Node \"Target\" list exceeds the maximum of %d, skipping.\n", MAX_QUEST_OBJECTIVES);
				return 0;
			}

			if (!this->nodeExists(targetNode, "Mob"))
				continue;

			std::string mob_name;

			if (!this->asString(targetNode, "Mob", mob_name))
				return 0;

			struct mob_db *mob = mobdb_search_aegisname(mob_name.c_str());

			if (!mob) {
				this->invalidWarning(targetNode["Mob"], "Mob %s does not exist.\n", mob_name.c_str());
				return 0;
			}

			//std::shared_ptr<s_quest_objective> target = util::vector_find(quest->objectives, mob->vd.class_);
			std::shared_ptr<s_quest_objective> target;
			std::vector<std::shared_ptr<s_quest_objective>>::iterator it = std::find_if(quest->objectives.begin(), quest->objectives.end(), [&](std::shared_ptr<s_quest_objective> const &v) {
				return (*v).mob_id == mob->vd.class_;
			});

			if (it != quest->objectives.end())
				target = (*it);
			else
				target = nullptr;

			bool targetExists = target != nullptr;

			if (!targetExists) {
				if (!this->nodeExists(targetNode, "Count")) {
					this->invalidWarning(targetNode, "Node \"Target\" has no data specified, skipping.\n");
					return 0;
				}

				target = std::make_shared<s_quest_objective>();
				target->mob_id = mob->vd.class_;
			}

			if (this->nodeExists(targetNode, "Count")) {
				uint16 count;

				if (!this->asUInt16(targetNode, "Count", count))
					return 0;

				target->count = count;
			}

			quest->objectives.push_back(target);
		}
	}

	if (this->nodeExists(node, "Drop")) {
		const YAML::Node &drops = node["Drop"];

		for (const YAML::Node &dropNode : drops) {
			if (quest->objectives.size() >= MAX_QUEST_OBJECTIVES) {
				this->invalidWarning(dropNode, "Node \"Target\" list exceeds the maximum of %d, skipping.\n", MAX_QUEST_OBJECTIVES);
				return 0;
			}

			uint32 mob_id = 0; // Can be 0 which means all monsters

			if (this->nodeExists(dropNode, "Mob")) {
				std::string mob_name;

				if (!this->asString(dropNode, "Mob", mob_name))
					return 0;

				struct mob_db *mob = mobdb_search_aegisname(mob_name.c_str());

				if (!mob) {
					this->invalidWarning(dropNode["Mob"], "Mob %s does not exist.\n", mob_name.c_str());
					return 0;
				}

				mob_id = mob->vd.class_;
			}

			//std::shared_ptr<s_quest_dropitem> target = util::vector_find(quest->dropitem, mob_id);
			std::shared_ptr<s_quest_dropitem> target;
			std::vector<std::shared_ptr<s_quest_dropitem>>::iterator it = std::find_if(quest->dropitem.begin(), quest->dropitem.end(), [&](std::shared_ptr<s_quest_dropitem> const &v) {
				return (*v).mob_id == mob_id;
			});

			if (it != quest->dropitem.end())
				target = (*it);
			else
				target = nullptr;

			bool targetExists = target != nullptr;

			if (!targetExists) {
				if (!this->nodeExists(dropNode, "Item") || !this->nodeExists(dropNode, "Rate")) {
					this->invalidWarning(dropNode, "Node \"Drop\" has no data specified, skipping.\n");
					return 0;
				}

				target = std::make_shared<s_quest_dropitem>();
				target->mob_id = mob_id;
			}

			if (this->nodeExists(dropNode, "Item")) {
				std::string item_name;

				if (!this->asString(dropNode, "Item", item_name))
					return 0;

				struct item_data *item = itemdb_search_aegisname(item_name.c_str());

				if (!item) {
					this->invalidWarning(dropNode["Item"], "Item %s does not exist.\n", item_name.c_str());
					return 0;
				}

				target->nameid = item->nameid;
			}

			if (this->nodeExists(dropNode, "Count")) {
				uint16 count;

				if (!this->asUInt16(dropNode, "Count", count))
					return 0;

				if (!itemdb_isstackable(target->nameid)) {
					this->invalidWarning(dropNode["Count"], "Item %s is not stackable, capping to 1.\n", itemdb_name(target->nameid));
					count = 1;
				}

				target->count = count;
			} else {
				if (!targetExists)
					target->count = 1;
			}

			if (this->nodeExists(dropNode, "Rate")) {
				uint16 rate;

				if (!this->asUInt16(dropNode, "Rate", rate))
					return 0;

				target->rate = rate;
			}

			quest->dropitem.push_back(target);
		}
	}

	if (!exists)
		this->put(quest_id, quest);

	return 1;
}

/**
 * Searches a quest by ID.
 * @param quest_id : ID to lookup
 * @return Quest entry or nullptr on failure
 */
std::shared_ptr<s_quest_db> quest_search(int quest_id)
{
	auto quest = quest_db.find(quest_id);

	if (!quest)
		return nullptr;

	return quest;
}

/**
 * Sends quest info to the player on login.
 * @param sd : Player's data
 * @return 0 in case of success, nonzero otherwise (i.e. the player has no quests)
 */
int quest_pc_login(struct map_session_data *sd)
{
	if (!sd->avail_quests)
		return 1;

	clif_quest_send_list(sd);

#if PACKETVER < 20141022
	clif_quest_send_mission(sd);

	//@TODO[Haru]: Is this necessary? Does quest_send_mission not take care of this?
	for (int i = 0; i < sd->avail_quests; i++)
		clif_quest_update_objective(sd, &sd->quest_log[i], 0);
#endif

	return 0;
}

/**
 * Adds a quest to the player's list.
 * New quest will be added as Q_ACTIVE.
 * @param sd : Player's data
 * @param quest_id : ID of the quest to add.
 * @return 0 in case of success, nonzero otherwise
 */
int quest_add(struct map_session_data *sd, int quest_id)
{
	std::shared_ptr<s_quest_db> qi = quest_search(quest_id);

	if (!qi) {
		ShowError("quest_add: quest %d not found in DB.\n", quest_id);
		return -1;
	}

	if (quest_check(sd, quest_id, HAVEQUEST) >= 0) {
		ShowError("quest_add: Character %d already has quest %d.\n", sd->status.char_id, quest_id);
		return -1;
	}

	int n = sd->avail_quests; //Insertion point

	sd->num_quests++;
	sd->avail_quests++;
	RECREATE(sd->quest_log, struct quest, sd->num_quests);

	//The character has some completed quests, make room before them so that they will stay at the end of the array
	if (sd->avail_quests != sd->num_quests)
		memmove(&sd->quest_log[n + 1], &sd->quest_log[n], sizeof(struct quest) * (sd->num_quests-sd->avail_quests));

	sd->quest_log[n] = {};
	sd->quest_log[n].quest_id = qi->id;

	if (qi->time)
		sd->quest_log[n].time = (uint32)(time(NULL) + qi->time);
	else if (qi->timeminute >= 0) { // quest time limit at DD:HH:MM
		time_t t = time(NULL);
		struct tm *lt = localtime(&t);
		uint32 q_hour = 0, q_minute = 0;

		if (qi->timehour >= 0) {
			uint32 my_hour = (lt->tm_hour) * 3600 + (lt->tm_min) * 60 + (lt->tm_sec);
			q_hour = qi->timehour * 3600 + qi->timeminute * 60;

			if (my_hour < q_hour)
				q_hour -= my_hour;
			else
				q_hour += 86400 - my_hour;
		}
		else {
			uint32 my_minute = (lt->tm_min) * 60 + (lt->tm_sec);
			q_minute = qi->timeminute * 60;

			if (my_minute < q_minute)
				q_minute -= my_minute;
			else
				q_minute += 3600 - my_minute;
		}
		sd->quest_log[n].time = (uint32)(t + (qi->timeday * 86400) + q_hour + q_minute);
	}

	sd->quest_log[n].state = Q_ACTIVE;
	sd->save_quest = true;

	clif_quest_add(sd, &sd->quest_log[n]);
	clif_quest_update_objective(sd, &sd->quest_log[n], 0);

	if( save_settings&CHARSAVE_QUEST )
		chrif_save(sd, CSAVE_NORMAL);

	return 0;
}

/**
 * Replaces a quest in a player's list with another one.
 * @param sd : Player's data
 * @param qid1 : Current quest to replace
 * @param qid2 : New quest to add
 * @return 0 in case of success, nonzero otherwise
 */
int quest_change(struct map_session_data *sd, int qid1, int qid2)
{
	std::shared_ptr<s_quest_db> qi = quest_search(qid2);

	if (!qi) {
		ShowError("quest_change: quest %d not found in DB.\n", qid2);
		return -1;
	}

	if (quest_check(sd, qid2, HAVEQUEST) >= 0) {
		ShowError("quest_change: Character %d already has quest %d.\n", sd->status.char_id, qid2);
		return -1;
	}

	if (quest_check(sd, qid1, HAVEQUEST) < 0) {
		ShowError("quest_change: Character %d doesn't have quest %d.\n", sd->status.char_id, qid1);
		return -1;
	}

	int i;

	ARR_FIND(0, sd->avail_quests, i, sd->quest_log[i].quest_id == qid1);
	if (i == sd->avail_quests) {
		ShowError("quest_change: Character %d has completed quest %d.\n", sd->status.char_id, qid1);
		return -1;
	}

	sd->quest_log[i] = {};
	sd->quest_log[i].quest_id = qi->id;

	if (qi->time)
		sd->quest_log[i].time = (uint32)(time(NULL) + qi->time);
	else if (qi->timeminute >= 0) { // quest time limit at DD:HH:MM
		time_t t = time(NULL);
		struct tm *lt = localtime(&t);
		uint32 q_hour = 0, q_minute = 0;

		if (qi->timehour >= 0) {
			uint32 my_hour = (lt->tm_hour) * 3600 + (lt->tm_min) * 60 + (lt->tm_sec);
			q_hour = qi->timehour * 3600 + qi->timeminute * 60;

			if (my_hour < q_hour)
				q_hour -= my_hour;
			else
				q_hour += 86400 - my_hour;
		}
		else {
			uint32 my_minute = (lt->tm_min) * 60 + (lt->tm_sec);
			q_minute = qi->timeminute * 60;

			if (my_minute < q_minute)
				q_minute -= my_minute;
			else
				q_minute += 3600 - my_minute;
		}
		sd->quest_log[i].time = (uint32)(t + (qi->timeday * 86400) + q_hour + q_minute);
	}

	sd->quest_log[i].state = Q_ACTIVE;
	sd->save_quest = true;

	clif_quest_delete(sd, qid1);
	clif_quest_add(sd, &sd->quest_log[i]);
	clif_quest_update_objective(sd, &sd->quest_log[i], 0);

	if( save_settings&CHARSAVE_QUEST )
		chrif_save(sd, CSAVE_NORMAL);

	return 0;
}

/**
 * Removes a quest from a player's list
 * @param sd : Player's data
 * @param quest_id : ID of the quest to remove
 * @return 0 in case of success, nonzero otherwise
 */
int quest_delete(struct map_session_data *sd, int quest_id)
{
	int i;

	//Search for quest
	ARR_FIND(0, sd->num_quests, i, sd->quest_log[i].quest_id == quest_id);
	if (i == sd->num_quests) {
		ShowError("quest_delete: Character %d doesn't have quest %d.\n", sd->status.char_id, quest_id);
		return -1;
	}

	if (sd->quest_log[i].state != Q_COMPLETE)
		sd->avail_quests--;

	if (i < --sd->num_quests) //Compact the array
		memmove(&sd->quest_log[i], &sd->quest_log[i + 1], sizeof(struct quest) * (sd->num_quests - i));

	if (sd->num_quests == 0) {
		aFree(sd->quest_log);
		sd->quest_log = NULL;
	} else
		RECREATE(sd->quest_log, struct quest, sd->num_quests);

	sd->save_quest = true;

	clif_quest_delete(sd, quest_id);

	if( save_settings&CHARSAVE_QUEST )
		chrif_save(sd, CSAVE_NORMAL);

	return 0;
}

/**
 * Map iterator subroutine to update quest objectives for a party after killing a monster.
 * @see map_foreachinrange
 * @param ap : Argument list, expecting:
 *   int Party ID
 *   int Mob ID
 */
int quest_update_objective_sub(struct block_list *bl, va_list ap)
{
	struct map_session_data *sd;
	int mob_id, party_id;

	nullpo_ret(bl);
	nullpo_ret(sd = (struct map_session_data *)bl);

	party_id = va_arg(ap,int);
	mob_id = va_arg(ap,int);

	if( !sd->avail_quests )
		return 0;
	if( sd->status.party_id != party_id )
		return 0;

	quest_update_objective(sd, mob_id);

	return 1;
}

/**
 * Updates the quest objectives for a character after killing a monster, including the handling of quest-granted drops.
 * @param sd : Character's data
 * @param mob_id : Monster ID
 */
void quest_update_objective(struct map_session_data *sd, int mob_id)
{
	for (int i = 0; i < sd->avail_quests; i++) {
		if (sd->quest_log[i].state == Q_COMPLETE) // Skip complete quests
			continue;

		std::shared_ptr<s_quest_db> qi = quest_search(sd->quest_log[i].quest_id);

		// Process quest objectives
		for (int j = 0; j < qi->objectives.size(); j++) {
			if (qi->objectives[j]->mob_id == mob_id && sd->quest_log[i].count[j] < qi->objectives[j]->count)  {
				sd->quest_log[i].count[j]++;
				sd->save_quest = true;
				clif_quest_update_objective(sd, &sd->quest_log[i], mob_id);
			}
		}

		// Process quest-granted extra drop bonuses
		for (const auto &it : qi->dropitem) {
			if (it->mob_id != 0 && it->mob_id != mob_id)
				continue;
			if (it->rate < 10000 && rnd()%10000 >= it->rate)
				continue; // TODO: Should this be affected by server rates?
			if (!itemdb_exists(it->nameid))
				continue;

			struct item item = {};

			item.nameid = it->nameid;
			item.identify = itemdb_isidentified(it->nameid);
			item.amount = it->count;
//#ifdef BOUND_ITEMS
//			item.bound = it.bound;
//#endif
//			if (it.isGUID)
//				item.unique_id = pc_generate_unique_id(sd);
			
			char temp;

			if ((temp = pc_additem(sd, &item, 1, LOG_TYPE_QUEST)) != ADDITEM_SUCCESS) // Failed to obtain the item
				clif_additem(sd, 0, 0, temp);
//			else if (it.isAnnounced || itemdb_exists(it.nameid)->flag.broadcast)
//				intif_broadcast_obtain_special_item(sd, it.nameid, it.mob_id, ITEMOBTAIN_TYPE_MONSTER_ITEM);
		}
	}
	pc_show_questinfo(sd);
}

/**
 * Updates a quest's state.
 * Only status of active and inactive quests can be updated. Completed quests can't (for now).
 * @param sd : Character's data
 * @param quest_id : Quest ID to update
 * @param qs : New quest state
 * @return 0 in case of success, nonzero otherwise
 * @author [Inkfish]
 */
int quest_update_status(struct map_session_data *sd, int quest_id, enum e_quest_state status)
{
	int i;

	ARR_FIND(0, sd->avail_quests, i, sd->quest_log[i].quest_id == quest_id);
	if (i == sd->avail_quests) {
		ShowError("quest_update_status: Character %d doesn't have quest %d.\n", sd->status.char_id, quest_id);
		return -1;
	}

	sd->quest_log[i].state = status;
	sd->save_quest = true;

	if (status < Q_COMPLETE) {
		clif_quest_update_status(sd, quest_id, status == Q_ACTIVE ? true : false);
		return 0;
	}

	// The quest is complete, so it needs to be moved to the completed quests block at the end of the array.
	if (i < (--sd->avail_quests)) {
		struct quest tmp_quest;

		memcpy(&tmp_quest, &sd->quest_log[i], sizeof(struct quest));
		memcpy(&sd->quest_log[i], &sd->quest_log[sd->avail_quests], sizeof(struct quest));
		memcpy(&sd->quest_log[sd->avail_quests], &tmp_quest, sizeof(struct quest));
	}

	clif_quest_delete(sd, quest_id);

	if (save_settings&CHARSAVE_QUEST)
		chrif_save(sd, CSAVE_NORMAL);

	return 0;
}

/**
 * Queries quest information for a character.
 * @param sd : Character's data
 * @param quest_id : Quest ID
 * @param type : Check type
 * @return -1 if the quest was not found, otherwise it depends on the type:
 *   HAVEQUEST: The quest's state
 *   PLAYTIME:  2 if the quest's timeout has expired
 *              1 if the quest was completed
 *              0 otherwise
 *   HUNTING:   2 if the quest has not been marked as completed yet, and its objectives have been fulfilled
 *              1 if the quest's timeout has expired
 *              0 otherwise
 */
int quest_check(struct map_session_data *sd, int quest_id, enum e_quest_check_type type)
{
	int i;

	ARR_FIND(0, sd->num_quests, i, sd->quest_log[i].quest_id == quest_id);
	if (i == sd->num_quests)
		return -1;

	switch (type) {
		case HAVEQUEST:
			if (sd->quest_log[i].state == Q_INACTIVE) // Player has the quest but it's in the inactive state; send it as Q_ACTIVE.
				return 1;
			return sd->quest_log[i].state;
		case PLAYTIME:
			return (sd->quest_log[i].time < (unsigned int)time(NULL) ? 2 : sd->quest_log[i].state == Q_COMPLETE ? 1 : 0);
		case HUNTING:
			if (sd->quest_log[i].state == Q_INACTIVE || sd->quest_log[i].state == Q_ACTIVE) {
				int j;
				std::shared_ptr<s_quest_db> qi = quest_search(sd->quest_log[i].quest_id);

				ARR_FIND(0, qi->objectives.size(), j, sd->quest_log[i].count[j] < qi->objectives[j]->count);
				if (j == qi->objectives.size())
					return 2;
				if (sd->quest_log[i].time < (unsigned int)time(NULL))
					return 1;
			}
			return 0;
		default:
			ShowError("quest_check_quest: Unknown parameter %d",type);
			break;
	}

	return -1;
}

/**
 * Map iterator to ensures a player has no invalid quest log entries.
 * Any entries that are no longer in the db are removed.
 * @see map_foreachpc
 * @param sd : Character's data
 * @param ap : Ignored
 */
static int quest_reload_check_sub(struct map_session_data *sd, va_list ap)
{
	nullpo_ret(sd);

	int i, j = 0;

	for (i = 0; i < sd->num_quests; i++) {
		std::shared_ptr<s_quest_db> qi = quest_search(sd->quest_log[i].quest_id);

		if (!qi) { //Remove no longer existing entries
			if (sd->quest_log[i].state != Q_COMPLETE) //And inform the client if necessary
				clif_quest_delete(sd, sd->quest_log[i].quest_id);
			continue;
		}

		if (i != j) {
			//Move entries if there's a gap to fill
			memcpy(&sd->quest_log[j], &sd->quest_log[i], sizeof(struct quest));
		}

		j++;
	}

	sd->num_quests = j;
	ARR_FIND(0, sd->num_quests, i, sd->quest_log[i].state == Q_COMPLETE);
	sd->avail_quests = i;

	return 1;
}

bool QuestDatabase::reload() {
	if (!TypesafeYamlDatabase::reload())
		return false;

	// Update quest data for players, to ensure no entries about removed quests are left over.
	map_foreachpc(&quest_reload_check_sub);
	return true;
}

QuestDatabase quest_db;

/**
 * Initializes the quest interface.
 */
void do_init_quest(void)
{
	quest_db.load();
}

/**
 * Finalizes the quest interface before shutdown.
 */
void do_final_quest(void)
{
}
