﻿/*
 * roe.cpp
 *      Author: Kreidos | github.com/kreidos
 *
===========================================================================

  Copyright (c) 2020 Topaz Dev Teams

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see http://www.gnu.org/licenses/

===========================================================================
*/
#include <ctime>

#include "lua/luautils.h"
#include "packets/chat_message.h"
#include "roe.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"
#include "vana_time.h"

#include "packets/roe_questlog.h"
#include "packets/roe_sparkupdate.h"
#include "packets/roe_update.h"

#define ROE_CACHETIME 15

std::array<RoeCheckHandler, ROE_NONE> RoeHandlers;
RoeSystemData                         roeutils::RoeSystem;

void SaveEminenceDataNice(CCharEntity* PChar)
{
    if (PChar->m_eminenceCache.lastWriteout < time(nullptr) - ROE_CACHETIME)
    {
        charutils::SaveEminenceData(PChar);
    }
}

void call_onRecordTrigger(CCharEntity* PChar, uint16 recordID, const RoeDatagramList& payload)
{
    // TODO: Move this Lua interaction into luautils
    auto onRecordTrigger = luautils::lua[sol::create_if_nil]["tpz"]["roe"]["onRecordTrigger"];
    if (!onRecordTrigger.valid())
    {
        sol::error err = onRecordTrigger;
        ShowError("roeutils::onRecordTrigger: record %d: %s\n.", recordID, err.what());
        return;
    }

    // Create param table
    auto params = luautils::lua.create_table();
    params["progress"] = roeutils::GetEminenceRecordProgress(PChar, recordID);

    for (auto& datagram : payload) // Append datagrams to param table
    {
        if (auto value = std::get_if<uint32>(&datagram.data))
        {
            params[datagram.luaKey] = *value;
        }
        else if (auto PMob = std::get_if<CMobEntity*>(&datagram.data))
        {
            params[datagram.luaKey] = CLuaBaseEntity(*PMob);
        }
        else if (auto text = std::get_if<std::string>(&datagram.data))
        {
            params[datagram.luaKey] = text;
        }
        else
        {
            ShowWarning("roeutils::onRecordTrigger: Unhandled payload type for '%s' with record #%d.", datagram.luaKey, recordID);
        }
    }

    // Call
    auto result = onRecordTrigger(CLuaBaseEntity(PChar), recordID, params);
    if (!result.valid())
    {
        sol::error err = result;
        ShowError("roeutils::onRecordTrigger: %s\n", err.what());
    }
}

namespace roeutils
{
    void init()
    {
        roeutils::RoeSystem.RoeEnabled = luautils::lua["ENABLE_ROE"].get_or(0);
        luautils::lua["RoeParseRecords"] = &roeutils::ParseRecords;
        luautils::lua["RoeParseTimed"] = &roeutils::ParseTimedSchedule;
        RoeHandlers.fill(RoeCheckHandler());
    }

    void ParseRecords(sol::table const& records_table)
    {
        RoeHandlers.fill(RoeCheckHandler());
        roeutils::RoeSystem.ImplementedRecords.reset();
        roeutils::RoeSystem.RepeatableRecords.reset();
        roeutils::RoeSystem.RetroactiveRecords.reset();
        roeutils::RoeSystem.DailyRecords.reset();
        roeutils::RoeSystem.DailyRecordIDs.clear();
        roeutils::RoeSystem.NotifyThresholds.fill(1);

        // TODO: Move this Lua interaction into luautils
        for (auto& entry : records_table)
        {
            // Set Implemented bit.
            uint16 recordID = entry.first.as<uint16>();
            auto   table    = entry.second.as<sol::table>();

            roeutils::RoeSystem.ImplementedRecords.set(recordID);

            // Register Trigger Handler
            if (table["trigger"].valid())
            {
                uint32 trigger = table["trigger"].get<uint32>();
                if (trigger > 0 && trigger < ROE_NONE)
                {
                    RoeHandlers[trigger].bitmap.set(recordID);
                }
                else
                {
                    ShowError("ROEUtils: Unknown Record trigger index %d for record %d.", trigger, recordID);
                }
            }

            // Set notification threshold
            if (table["notify"].valid())
            {
                roeutils::RoeSystem.NotifyThresholds[recordID] = table["notify"].get<uint32>();
            }

            // Set flags
            auto flags = table["flags"].get<sol::table>();
            if (flags.valid())
            {
                for (auto& flag_entry : flags)
                {
                    std::string flag = flag_entry.first.as<std::string>();
                    if (flag == "daily")
                    {
                        roeutils::RoeSystem.DailyRecords.set(recordID);
                        roeutils::RoeSystem.DailyRecordIDs.push_back(recordID);
                    }
                    else if (flag == "timed")
                    {
                        roeutils::RoeSystem.TimedRecords.set(recordID);
                    }
                    else if (flag == "repeat")
                    {
                        roeutils::RoeSystem.RepeatableRecords.set(recordID);
                    }
                    else if (flag == "retro")
                    {
                        roeutils::RoeSystem.RetroactiveRecords.set(recordID);
                    }
                    else
                    {
                        ShowError("ROEUtils: Unknown flag %s for record #%d.", flag, recordID);
                    }
                }
            }
        }
        // ShowInfo("\nRoEUtils: %d record entries parsed & available.", RoeBitmaps.ImplementedRecords.count());
    }

    void ParseTimedSchedule(sol::table const& schedule_table)
    {
        roeutils::RoeSystem.TimedRecords.reset();
        roeutils::RoeSystem.TimedRecordTable.fill(RecordTimetable_D{});

        for (auto& entry : schedule_table)
        {
            uint8 day       = entry.first.as<uint8>() - 1;
            auto  timeslots = entry.second.as<sol::table>();
            for (auto slot_entry : timeslots)
            {
                auto   block    = slot_entry.first.as<uint16>() - 1;
                uint16 recordID = slot_entry.second.as<uint16>();

                roeutils::RoeSystem.TimedRecordTable.at(day).at(block) = recordID;
            }
        }
    }

    bool event(ROE_EVENT eventID, CCharEntity* PChar, const RoeDatagramList& payload)
    {
        TracyZoneScoped;
        if (!RoeSystem.RoeEnabled || !PChar || PChar->objtype != TYPE_PC)
        {
            return false;
        }

        RoeCheckHandler& handler = RoeHandlers[eventID];

        // Bail if player has no records of this type.
        if ((PChar->m_eminenceCache.activemap & handler.bitmap).none())
        {
            return false;
        }

        // Call onRecordTrigger for each record of this type
        for (int i = 0; i < 31; i++)
        {
            // Check record is of this type
            if (uint16 recordID = PChar->m_eminenceLog.active[i]; handler.bitmap.test(recordID))
            {
                call_onRecordTrigger(PChar, recordID, payload);
            }
        }

        return true;
    }

    bool event(ROE_EVENT eventID, CCharEntity* PChar, const RoeDatagram& data) // shorthand for single-datagram calls.
    {
        return event(eventID, PChar, RoeDatagramList{ data });
    }

    void SetEminenceRecordCompletion(CCharEntity* PChar, uint16 recordID, bool newStatus)
    {
        uint16 page = recordID / 8;
        uint8  bit  = recordID % 8;
        if (newStatus)
        {
            PChar->m_eminenceLog.complete[page] |= (1 << bit);
        }
        else
        {
            PChar->m_eminenceLog.complete[page] &= ~(1 << bit);
        }

        for (int i = 0; i < 4; i++)
        {
            PChar->pushPacket(new CRoeQuestLogPacket(PChar, i));
        }
        charutils::SaveEminenceData(PChar);
    }

    bool GetEminenceRecordCompletion(CCharEntity* PChar, uint16 recordID)
    {
        uint16 page = recordID / 8;
        uint8  bit  = recordID % 8;
        return PChar->m_eminenceLog.complete[page] & (1 << bit);
    }

    bool AddEminenceRecord(CCharEntity* PChar, uint16 recordID)
    {
        // We deny taking records which aren't implemented in the Lua
        if (!roeutils::RoeSystem.ImplementedRecords.test(recordID))
        {
            std::string message = "The record #" + std::to_string(recordID) + " is not implemented at this time.";
            PChar->pushPacket(new CChatMessagePacket(PChar, MESSAGE_NS_SAY, message, "RoE System"));
            return false;
        }

        // Prevent packet-injection for re-taking completed records which aren't marked repeatable.
        if (roeutils::GetEminenceRecordCompletion(PChar, recordID) && !roeutils::RoeSystem.RepeatableRecords.test(recordID))
        {
            return false;
        }

        // Prevent packet-injection from taking timed records as normal ones.
        if (roeutils::RoeSystem.TimedRecords.test(recordID))
        {
            return false;
        }

        for (int i = 0; i < 30; i++)
        {
            if (PChar->m_eminenceLog.active[i] == 0)
            {
                PChar->m_eminenceLog.active[i] = recordID;
                PChar->m_eminenceCache.activemap.set(recordID);

                PChar->pushPacket(new CRoeUpdatePacket(PChar));
                PChar->pushPacket(new CMessageBasicPacket(PChar, PChar, recordID, 0, MSGBASIC_ROE_START));
                charutils::SaveEminenceData(PChar);
                return true;
            }
            else if (PChar->m_eminenceLog.active[i] == recordID)
            {
                return false;
            }
        }
        return false;
    }

    bool DelEminenceRecord(CCharEntity* PChar, uint16 recordID)
    {
        for (int i = 0; i < 30; i++)
        {
            if (PChar->m_eminenceLog.active[i] == recordID)
            {
                PChar->m_eminenceLog.active[i]   = 0;
                PChar->m_eminenceLog.progress[i] = 0;
                PChar->m_eminenceCache.activemap.reset(recordID);
                // Shift entries up so records are shown in retail-accurate order.
                for (int j = i; j < 29 && PChar->m_eminenceLog.active[j + 1] != 0; j++)
                {
                    std::swap(PChar->m_eminenceLog.active[j], PChar->m_eminenceLog.active[j + 1]);
                    std::swap(PChar->m_eminenceLog.progress[j], PChar->m_eminenceLog.progress[j + 1]);
                }
                PChar->pushPacket(new CRoeUpdatePacket(PChar));
                charutils::SaveEminenceData(PChar);
                return true;
            }
        }
        return false;
    }

    bool HasEminenceRecord(CCharEntity* PChar, uint16 recordID)
    {
        return PChar->m_eminenceCache.activemap.test(recordID);
    }

    uint32 GetEminenceRecordProgress(CCharEntity* PChar, uint16 recordID)
    {
        for (int i = 0; i < 31; i++)
        {
            if (PChar->m_eminenceLog.active[i] == recordID)
            {
                return PChar->m_eminenceLog.progress[i];
            }
        }
        return 0;
    }

    bool SetEminenceRecordProgress(CCharEntity* PChar, uint16 recordID, uint32 progress)
    {
        for (int i = 0; i < 31; i++)
        {
            if (PChar->m_eminenceLog.active[i] == recordID)
            {
                if (PChar->m_eminenceLog.progress[i] == progress)
                {
                    return true;
                }

                PChar->m_eminenceLog.progress[i] = progress;
                PChar->pushPacket(new CRoeUpdatePacket(PChar));
                SaveEminenceDataNice(PChar);
                return true;
            }
        }
        return false;
    }

    void onCharLoad(CCharEntity* PChar)
    {
        if (!RoeSystem.RoeEnabled)
        {
            return;
        }

        // Build eminence lookup map
        for (unsigned short record : PChar->m_eminenceLog.active)
        {
            if (record)
            {
                PChar->m_eminenceCache.activemap.set(record);
            }
        }

        // Only chars with First Step Forward complete can get timed/daily records
        if (GetEminenceRecordCompletion(PChar, 1))
        {
            // Time gets messy, avert your eyes.
            auto jstnow     = time(nullptr) + JST_OFFSET;
            auto lastOnline = PChar->lastOnline;

            { // Daily Reset
                auto* jst            = gmtime(&jstnow);
                jst->tm_hour         = 0;
                jst->tm_min          = 0;
                jst->tm_sec          = 0;
                auto lastJstMidnight = timegm(jst) - JST_OFFSET; // Unix timestamp of the last JST midnight

                if (lastOnline < lastJstMidnight)
                {
                    ClearDailyRecords(PChar);
                }
            }

            { // 4hr Reset
                auto* jst              = gmtime(&jstnow);
                jst->tm_hour           = jst->tm_hour & 0xFC;
                jst->tm_min            = 0;
                jst->tm_sec            = 0;
                auto lastJstTimedBlock = timegm(jst) - JST_OFFSET; // Unix timestamp of the start of the current 4-hr block

                if (lastOnline < lastJstTimedBlock || PChar->m_eminenceLog.active[30] != GetActiveTimedRecord())
                {
                    PChar->m_eminenceCache.notifyTimedRecord = static_cast<bool>(GetActiveTimedRecord());
                    AddActiveTimedRecord(PChar);
                }
            }
        }
    }

    void onRecordTake(CCharEntity* PChar, uint16 recordID)
    {
        if (RoeSystem.RetroactiveRecords.test(recordID))
        {
            call_onRecordTrigger(PChar, recordID, RoeDatagramList{});
        }
        return;
    }

    bool onRecordClaim(CCharEntity* PChar, uint16 recordID)
    {
        if (roeutils::HasEminenceRecord(PChar, recordID))
        {
            call_onRecordTrigger(PChar, recordID, RoeDatagramList{ RoeDatagram("claim", 1) });
            return true;
        }
        return false;
    }

    uint16 GetActiveTimedRecord()
    {
        uint8 day   = static_cast<uint8>(CVanaTime::getInstance()->getJstWeekDay());
        uint8 block = static_cast<uint8>(CVanaTime::getInstance()->getJstHour() / 4);
        return RoeSystem.TimedRecordTable[day][block];
    }

    void AddActiveTimedRecord(CCharEntity* PChar)
    {
        // Clear old timed entries from log
        PChar->m_eminenceLog.progress[30] = 0;
        PChar->m_eminenceCache.activemap &= ~RoeSystem.TimedRecords;

        // Add current timed record to slot 31
        auto timedRecordID              = GetActiveTimedRecord();
        PChar->m_eminenceLog.active[30] = timedRecordID;
        PChar->m_eminenceCache.activemap.set(timedRecordID);
        PChar->pushPacket(new CRoeUpdatePacket(PChar));

        if (timedRecordID)
        {
            PChar->pushPacket(new CMessageBasicPacket(PChar, PChar, timedRecordID, 0, MSGBASIC_ROE_TIMED));
            SetEminenceRecordCompletion(PChar, timedRecordID, false);
        }
    }

    void ClearDailyRecords(CCharEntity* PChar)
    {
        // Set daily record progress to 0
        for (int i = 0; i < 30; i++)
        {
            if (auto recordID = PChar->m_eminenceLog.active[i]; RoeSystem.DailyRecords.test(recordID))
            {
                PChar->m_eminenceLog.progress[i] = 0;
            }
        }
        PChar->pushPacket(new CRoeUpdatePacket(PChar));

        // Set completion for daily records to 0
        for (auto record : RoeSystem.DailyRecordIDs)
        {
            uint16 page = record / 8;
            uint8  bit  = record % 8;
            PChar->m_eminenceLog.complete[page] &= ~(1 << bit);
        }

        charutils::SaveEminenceData(PChar);

        for (int i = 0; i < 4; i++)
        {
            PChar->pushPacket(new CRoeQuestLogPacket(PChar, i));
        }
    }

    void CycleTimedRecords()
    {
        TracyZoneScoped;
        if (!RoeSystem.RoeEnabled)
        {
            return;
        }

        zoneutils::ForEachZone([](CZone* PZone) {
            PZone->ForEachChar([](CCharEntity* PChar) {
                if (GetEminenceRecordCompletion(PChar, 1))
                {
                    AddActiveTimedRecord(PChar);
                }
            });
        });
    }

    void CycleDailyRecords()
    {
        TracyZoneScoped;
        if (!RoeSystem.RoeEnabled)
        {
            return;
        }

        zoneutils::ForEachZone([](CZone* PZone) { PZone->ForEachChar([](CCharEntity* PChar) { ClearDailyRecords(PChar); }); });
    }

} // namespace roeutils
