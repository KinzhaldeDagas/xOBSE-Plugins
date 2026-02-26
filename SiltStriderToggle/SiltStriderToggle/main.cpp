#include "obse/PluginAPI.h"
#include "obse/GameAPI.h"
#include "obse/GameForms.h"
#include "obse/GameObjects.h"
#include "obse/GameRTTI.h"
#include "obse/Tasks.h"
#include "obse/GameReferences.h"
#include "obse/GameData.h"

PluginHandle g_pluginHandle = kPluginHandle_Invalid;
OBSETasks2Interface* g_tasks = nullptr;

TESFaction* g_striderFaction = nullptr; // mwStriderFaction 01000D46
TESTopic* g_destinationTopic = nullptr; // AAADestination 01000D47

void __cdecl UpdateStriderTopic()
{
    TES* tes = TES::GetSingleton();
    if (!tes || !tes->gridCellArray) return;

    for (UInt32 i = 0; i < tes->gridCellArray->size * tes->gridCellArray->size; ++i)
    {
        TESObjectCELL* cell = tes->gridCellArray->grid[i].cell;
        if (!cell) continue;

        for (TESObjectREFR* ref = cell->objectList.first; ref; ref = ref->next)
        {
            TESNPC* npc = DYNAMIC_CAST(ref->baseForm, TESForm, TESNPC);
            if (!npc || !npc->factionList) continue;

            for (UInt32 j = 0; j < npc->factionList->count; ++j)
            {
                TESFaction* faction = nullptr;
                npc->factionList->GetNthItem(j, faction);
                if (faction == g_striderFaction)
                {
                    ref->Enable();
                    break;
                }
            }
        }
    }
}

extern "C"
{

    bool OBSEPlugin_Query(const OBSEInterface* obse, PluginInfo* info)
    {
        info->infoVersion = PluginInfo::kInfoVersion;
        info->name = "SiltStriderTopicToggle";
        info->version = 1;

        if (obse->oblivionVersion < OBLIVION_VERSION_1_2_416)
            return false;

        g_pluginHandle = obse->GetPluginHandle();
        return true;
    }

    bool OBSEPlugin_Load(const OBSEInterface* obse)
    {
        g_tasks = (OBSETasks2Interface*)obse->QueryInterface(kInterface_Tasks2);
        if (!g_tasks)
            return false;

        g_striderFaction = (TESFaction*)Oblivion_DynamicCast(
            LookupFormByID(0x01000D46), 0, RTTI_TESForm, RTTI_TESFaction, 0);
        g_destinationTopic = (TESTopic*)LookupFormByID(0x01000D47);

        if (!g_striderFaction || !g_destinationTopic)
            return false;

        g_tasks->EnqueueTask(UpdateStriderTopic);
        return true;
    }

}
