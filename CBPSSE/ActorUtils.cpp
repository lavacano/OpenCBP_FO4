#include <algorithm>
#include <atomic>
#include <concurrent_unordered_set.h>
#include <unordered_map>

#include "ActorUtils.h"
#include "log.h"
#include "unordered_dense.h"

#include "f4se/GameExtraData.h"
#include "f4se/GameObjects.h"
#include <f4se/GameReferences.h>
#include "f4se/GameRTTI.h"

#include <f4se/NiNodes.h>
#include <f4se/NiObjects.h>
#include <f4se/NiTypes.h>

std::string actorUtils::GetActorRaceEID(Actor* actor)
{
    if (!IsActorValid(actor))
    {
        logger.Info("GetActorRaceEID: no actor!\n");
        return "";
    }

    return std::string(actor->race->editorId.c_str());
}

// One-shot scene-graph dump so cbp.log shows what the actor's 3D actually
// looks like on this game version. Fires once for the first failure and once
// for the first success, then stays quiet.
static void DumpNodeTree(NiAVObject* node, int depth, int maxDepth)
{
    if (!node) return;
    const char* nm = node->m_name.c_str();
    NiNode* asNode = node->GetAsNiNode();
    logger.Error("    %*s%s%s\n", depth * 2, "", nm ? nm : "<unnamed>", asNode ? "/" : "");
    if (!asNode || depth >= maxDepth) return;
    for (UInt32 i = 0; i < asNode->m_children.m_emptyRunStart; i++)
    {
        NiAVObject* child = asNode->m_children.m_data[i];
        if (child) DumpNodeTree(child, depth + 1, maxDepth);
    }
}

NiAVObject* actorUtils::GetBaseSkeleton(Actor* actor)
{
    static BSFixedString skeletonNif_name("skeleton.nif");
    static std::atomic<bool> dumpedFailure = false;
    static std::atomic<bool> dumpedSuccess = false;
    static concurrency::concurrent_unordered_set<UInt32> loggedFailure;

    if (!actorUtils::IsActorValid(actor))
    {
        logger.Error("%s: No valid actor\n", __func__);
        return NULL;
    }
    auto loadedState = actor->unkF0;
    if (!loadedState || !loadedState->rootNode)
    {
        logger.Error("%s:No loaded state for actor %08x\n", __func__, actor->formID);
        return NULL;
    }

    NiNode* root = loadedState->rootNode;
    const char* how = "child";
    NiAVObject* obj = root->GetObjectByName(&skeletonNif_name);

    // GetObjectByName searches descendants; if the loaded root IS skeleton.nif,
    // accept the root itself.
    if (!obj && root->m_name.c_str() && _stricmp(root->m_name.c_str(), "skeleton.nif") == 0)
    {
        obj = root;
        how = "root";
    }

    if (!obj)
    {
        if (loggedFailure.insert(actor->formID).second)
        {
            logger.Error("%s: no skeleton.nif under root '%s' for actor %08x (further failures for this actor not logged)\n",
                         __func__, root->m_name.c_str() ? root->m_name.c_str() : "<unnamed>", actor->formID);
        }
        if (!dumpedFailure.exchange(true))
        {
            logger.Error("%s: scene graph dump (FAILURE) for actor %08x, root parent '%s':\n", __func__, actor->formID,
                         (root->m_parent && root->m_parent->m_name.c_str()) ? root->m_parent->m_name.c_str() : "<none>");
            DumpNodeTree(root, 0, 3);
        }
        return NULL;
    }

    if (!dumpedSuccess.exchange(true))
    {
        logger.Error("%s: found skeleton.nif as %s for actor %08x; scene graph dump (SUCCESS), root parent '%s':\n", __func__, how, actor->formID,
                     (root->m_parent && root->m_parent->m_name.c_str()) ? root->m_parent->m_name.c_str() : "<none>");
        DumpNodeTree(root, 0, 3);
    }

    return obj;
}

bool actorUtils::IsActorFilteredActor(Actor* actor, UInt32 priority)
{
    auto& actorOverrideConfigEntry = configActorOverrideMap[priority];
    auto& overrideActors = actorOverrideConfigEntry.actors;
    auto isFilterInverted = actorOverrideConfigEntry.isFilterInverted;
    bool result;

    if (isFilterInverted)
    {
        logger.Info("%s: actor %08x is actor whitelisted\n", __func__, actor->formID);
        // Result is in the whitelist
        result = !(overrideActors.count(actor->formID) > 0);
    }
    else
    {
        logger.Info("%s: actor %08x is actor blacklisted\n", __func__, actor->formID);
        // Result is in the blacklist
        result = (overrideActors.count(actor->formID) > 0);
    }

    // Simplified to xor
    //result = isFilterInverted ^ (overrideActors.count(actor->formID) == 0);
    
    return result;
}

bool actorUtils::IsActorMale(Actor* actor)
{
    if (!IsActorValid(actor))
    {
        logger.Info("IsActorMale: no actor!\n");
        return false;
    }

    TESNPC* actorNPC = DYNAMIC_CAST(actor->baseForm, TESForm, TESNPC);

    auto npcSex = actorNPC ? CALL_MEMBER_FN(actorNPC, GetSex)() : 1;

    if (npcSex == 0) //Actor is male
        return true;
    else
        return false;
}

bool actorUtils::IsActorInPowerArmor(Actor* actor)
{
    bool isInPowerArmor = false;
    if (!IsActorValid(actor))
    {
        logger.Info("IsActorInPowerArmor: no actor!\n");
        return true;
    }
    if (!actor->extraDataList)
    {
        logger.Info("IsActorInPowerArmor: no extraDataList!\n");
        return true;
    }

    isInPowerArmor = actor->extraDataList->HasType(kExtraData_PowerArmor);
    //logger.Info("is in power armor: %d\n", isInPowerArmor);
    return isInPowerArmor;
}

bool actorUtils::IsActorTrackable(Actor* actor)
{
    if (!IsActorValid(actor))
    {
        logger.Info("%s: actor %x is not trackable.\n", __func__, actor->formID);
        return false;
    }

    bool inRaceWhitelist = find(raceWhitelist.begin(), raceWhitelist.end(), actorUtils::GetActorRaceEID(actor)) != raceWhitelist.end();
    return (!playerOnly || (actor->formID == 0x14 && playerOnly)) &&
        (!maleOnly || (IsActorMale(actor) && maleOnly)) &&
        (!femaleOnly || (!IsActorMale(actor) && femaleOnly)) &&
        (!npcOnly || (actor->formID != 0x14 && npcOnly)) &&
        (!useWhitelist || (inRaceWhitelist && useWhitelist));
}

bool actorUtils::IsActorValid(Actor* actor)
{
    if (!actor)
    {
        logger.Info("%s: actor %x is null\n", __func__, actor->formID);
        return false;
    }
    if (actor->flags & TESForm::kFlag_IsDeleted)
    {
        logger.Info("%s: actor %x has deleted flag\n", __func__, actor->formID);
        return false;
    }
    if (actor->unkF0 && actor->unkF0->rootNode)
    {
        return true;
    }
    logger.Info("%s: actor %x is not in a valid state\n", __func__, actor->formID);
    return false;
}

bool actorUtils::IsBoneInWhitelist(Actor* actor, std::string boneName)
{
    if (!IsActorValid(actor))
    {
        logger.Info("IsBoneInWhitelist: actor is not valid.\n");
        return false;
    }
    bool result;
    auto raceEID = actorUtils::GetActorRaceEID(actor);
    auto whitelist_bone = whitelist.find(boneName);
    if (whitelist_bone != whitelist.end())
    {
        auto & racesMap = whitelist_bone->second;

        if (racesMap.find(raceEID) != racesMap.end())
        {
            if (IsActorMale(actor))
            {
                result = racesMap.at(raceEID).male;
                //logger.Info("%s: %s male is %d for actor %x.\n", __func__, boneName.c_str(), result,  actor->formID);
                return result;
            }
            else
            {
                result = racesMap.at(raceEID).female;
                //logger.Info("%s: %s female is %d for actor %x.\n", __func__, boneName.c_str(), result, actor->formID);
                return result;
            }
        }
    }
    return false;
}

const actorUtils::EquippedArmor actorUtils::GetActorEquippedArmor(Actor* actor, UInt32 slot)
{
    // F4SE 0.7.9: the old Actor::equipData (ActorEquipData::slots[]) is gone.
    // The third-person biped (Actor::biped at 0x428) holds the same per-slot
    // data: parent.object is the worn item, armorAddon is what used to be "model".
    if (!actorUtils::IsActorValid(actor))
    {
        logger.Error("Actor is not valid");
        return actorUtils::EquippedArmor{ nullptr, nullptr };
    }
    if (slot >= BIPOBJECT::BIPED_OBJECT::kTotal)
    {
        logger.Error("slot %u out of range", slot);
        return actorUtils::EquippedArmor{ nullptr, nullptr };
    }

    BipedAnim* biped = actor->biped.get();
    if (!biped)
    {
        logger.Info("Actor %08x has no biped data", actor->formID);
        return actorUtils::EquippedArmor{ nullptr, nullptr };
    }

    const BIPOBJECT& obj = biped->object[slot];
    if (!obj.parent.object)
    {
        return actorUtils::EquippedArmor{ nullptr, nullptr };
    }
    return actorUtils::EquippedArmor{ obj.parent.object, obj.armorAddon };
}

UInt64 actorUtils::BuildActorKey(Actor* actor)
{
    std::unordered_map<UInt64, UInt64> key;
    ankerl::unordered_dense::hash<UInt64> hash;
    UInt64 hashKey = 0;
    UInt64 actorFormID = (UInt64)actor->formID;

    // key always starts with actors formID (really the refID)
    key[actorFormID] = 1;
    hashKey = hash((UInt64)actor->formID);

    for (auto slot : usedSlots)
    {
        // Check if actor has config's slot equipped
        UInt64 data = 0;
        auto equipped = actorUtils::GetActorEquippedArmor(actor, slot);

        // If there were any slots found, store it
        if (equipped.armor)
        {
            data |= equipped.armor->formID;
            data = data << 32;
        }
        if (equipped.model)
        {
            data |= equipped.model->formID;
        }
        if (data)
        {   
            auto valIter = key.find(data);
            if (valIter != key.end())
            {
                key[data] = key[data] + 1;
                hashKey += hash(data) * key[data];
            }
            else
            {
                key[data] = 1;
                hashKey += hash(data) * key[data];
            }
        }
    }

    //logger.Info("%s: actor %08x has hash key 0x%x\n", __func__, actor->formID, hashKey);

    return hashKey;
}

config_t actorUtils::BuildConfigForActor(Actor* actor, UInt64 hashKey)
{
    // Search cached configs for already existing config
    auto found = cachedConfigs.find(hashKey);
    if (found != cachedConfigs.end())
    {
        logger.Info("%s: Cached config found for actor %08x: %x\n", __func__, actor->formID, hashKey);
        return found->second;
    }

    // Otherwise, build the actor's config
    config_t baseConfig = config;

    for (auto priIter = priorities.rbegin(); priIter != priorities.rend(); ++priIter)
    {
        UInt32 priority = *priIter;

        // Check if there is no armor slot override entry
        if (configArmorOverrideMap.count(priority) == 0
            || (configArmorOverrideMap[priority].armors.empty()
                && configArmorOverrideMap[priority].slots.empty()))
        {
            auto & overrideConfig = configActorOverrideMap[priority].config;

            if (IsActorFilteredActor(actor, priority))
            {
                for (auto & val : overrideConfig)
                {
                    // for each bone, if it is empty, we need to disable it,
                    // otherwise the configEntry is good.
                    if (overrideConfig[val.first].empty())
                    {
                        // This is ok because we're doing this to a premade copy sequentially
                        baseConfig.unsafe_erase(val.first);
                    }
                    else
                    {
                        baseConfig[val.first] = val.second;
                    }
                }
            }
            else
            {
                logger.Info("%s: actor %08x is not filtered for priority %d\n", __func__, actor->formID, priority);
            }
        }
        else // There is an armor slot override entry
        {
            // If priority level has an entry in actor override map AND 
            // actor is not whitelisted or is blacklisted, continue on
            if (configActorOverrideMap.count(priority) > 0)
            {
                if (IsActorFilteredActor(actor, priority))
                {
                    logger.Info("%s: actor %08x is filtered for priority %d\n", __func__, actor->formID, priority);
                    continue;
                }
            }

            auto & orData = configArmorOverrideMap[priority];

            std::vector<actorUtils::EquippedArmor> equippedList;

            // Make a list of actor's slots that are config's slots
            for (auto slot : orData.slots)
            {
                EquippedArmor equipped = actorUtils::GetActorEquippedArmor(actor, slot);
                if (equipped.armor && equipped.model)
                {
                    equippedList.push_back(equipped);
                }
            }

            // whitelist filter
            if (orData.isFilterInverted)
            {
                logger.Info("%s: actor %08x is armor whitelisted\n", __func__, actor->formID);

                //  Check config's filter IDs against found slot's IDs 
                for (auto & equipped : equippedList)
                {
                    // Filter all armors listed
                    if (orData.armors.count(equipped.armor->formID) ||
                        orData.armors.count(equipped.model->formID))
                    {
                        for (auto & val : orData.config)
                        {
                            // for each bone, if it is empty, we need to disable it,
                            // otherwise the configEntry is good.
                            if (orData.config[val.first].empty())
                            {
                                // This is ok because we're doing this to a premade copy sequentially
                                baseConfig.unsafe_erase(val.first);
                            }
                            else
                            {
                                baseConfig[val.first] = val.second;
                            }
                        }
                    }
                }
            }

            // blacklist filter
            if (!orData.isFilterInverted && !equippedList.empty())
            {
                logger.Info("%s: actor %08x is armor blacklisted\n", __func__, actor->formID);

                //  Check config's filter IDs against found slot's IDs 
                for (auto& equipped : equippedList)
                {
                    // Filter all armors not listed
                    if (orData.armors.count(equipped.armor->formID) == 0 &&
                        orData.armors.count(equipped.model->formID) == 0)
                    {
                        for (auto& val : orData.config)
                        {
                            // for each bone, if it is empty, we need to disable it,
                            // otherwise the configEntry is good.
                            if (orData.config[val.first].empty())
                            {
                                // This is ok because we're doing this to a premade copy sequentially
                                baseConfig.unsafe_erase(val.first);
                            }
                            else
                            {
                                baseConfig[val.first] = val.second;
                            }
                        }
                    }
                }
            }
        }
    }

    logger.Info("%s: Inserting cached config for actor %08x: %x\n", __func__, actor->formID, hashKey);
    cachedConfigs.insert(std::make_pair(hashKey, baseConfig));
    //logger.Info("%s: exiting\n", __func__);
    return baseConfig;
}

namespace
{
    struct KeywordTierState
    {
        actorUtils::ArmorSupportTier tier{ 1.0f, 1.0f, 1.0f, 1.0f };
        bool isPowerArmor = false;
        bool hasSupportKeyword = false;
        bool hasExplicitLewd = false;
    };

    void ApplyTighterTier(actorUtils::ArmorSupportTier& dst, const actorUtils::ArmorSupportTier& cand)
    {
        if (cand.scaleMul < dst.scaleMul) dst.scaleMul = cand.scaleMul;
        if (cand.stiffnessMul > dst.stiffnessMul) dst.stiffnessMul = cand.stiffnessMul;
        if (cand.dampingMul > dst.dampingMul) dst.dampingMul = cand.dampingMul;
        if (cand.gravityMul < dst.gravityMul) dst.gravityMul = cand.gravityMul;
    }

    bool ContainsAnyKeyword(const char* kw, const char* const* needles, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            if (strstr(kw, needles[i]) != nullptr)
                return true;
        }
        return false;
    }

    struct KeywordTierRule
    {
        const char* const* keywords;
        size_t count;
        actorUtils::ArmorSupportTier tier;
    };

    void ApplyTierRules(const char* kw, const KeywordTierRule* rules, size_t ruleCount,
        const char* const* lewdKws, size_t lewdCount, float powerArmorGravity, KeywordTierState& state)
    {
        if (strstr(kw, "ArmorTypePower"))
        {
            state.isPowerArmor = true;
            state.tier = actorUtils::ArmorSupportTier{ 0.0f, 10.0f, 5.0f, powerArmorGravity };
            return;
        }

        for (size_t i = 0; i < ruleCount; ++i)
        {
            if (ContainsAnyKeyword(kw, rules[i].keywords, rules[i].count))
            {
                ApplyTighterTier(state.tier, rules[i].tier);
                state.hasSupportKeyword = true;
                return;
            }
        }

        if (ContainsAnyKeyword(kw, lewdKws, lewdCount))
            state.hasExplicitLewd = true;
    }

    void ClassifyBreastKeyword(const char* kw, KeywordTierState& state)
    {
        static const char* const kHeavyKws[] = {
            "sakr_kwd_armorTopFull", "ArmorTypeHeavy", "ArmorTypeMetal", "ArmorTypeCombat"
        };
        static const char* const kMediumKws[] = {
            "sakr_kwd_topFull", "sakr_kwd_armorTopBreast", "ArmorTypeMedium", "ArmorTypeLeather"
        };
        static const char* const kBraKws[] = {
            "sakr_kwd_braNormal", "sakr_kwd_topTagTight", "ArmorTypeLight"
        };
        static const char* const kClothKws[] = { "ArmorClothing" };
        static const char* const kSkimpyKws[] = {
            "sakr_kwd_braBikini", "sakr_kwd_topCleavage", "sakr_kwd_topLowCutCleavage",
            "sakr_kwd_topTagCropTop", "sakr_kwd_topTagTankTop", "sakr_kwd_topTagHalterTop",
            "sakr_kwd_topTagSideBoob", "sakr_kwd_braTagSheer", "sakr_kwd_topTagSheer"
        };
        static const char* const kLewdKws[] = {
            "sakr_kwd_braMicroBikini", "sakr_kwd_armorTopLewd"
        };
        // Normal clothing implies at least standard bra support underneath
        static const KeywordTierRule kRules[] = {
            { kHeavyKws,  sizeof(kHeavyKws)  / sizeof(kHeavyKws[0]),  { 0.40f, 1.60f, 1.30f, 0.35f } },
            { kMediumKws, sizeof(kMediumKws) / sizeof(kMediumKws[0]), { 0.58f, 1.30f, 1.18f, 0.50f } },
            { kBraKws,    sizeof(kBraKws)    / sizeof(kBraKws[0]),    { 0.72f, 1.15f, 1.10f, 0.62f } },
            { kClothKws,  sizeof(kClothKws)  / sizeof(kClothKws[0]),  { 0.78f, 1.12f, 1.08f, 0.68f } },
            { kSkimpyKws, sizeof(kSkimpyKws) / sizeof(kSkimpyKws[0]), { 0.86f, 1.06f, 1.04f, 0.80f } },
        };

        ApplyTierRules(kw, kRules, sizeof(kRules) / sizeof(kRules[0]),
            kLewdKws, sizeof(kLewdKws) / sizeof(kLewdKws[0]), 0.0f, state);
    }

    void ClassifyButtKeyword(const char* kw, KeywordTierState& state)
    {
        static const char* const kHeavyKws[] = {
            "sakr_kwd_armorBottomFull", "ArmorTypeHeavy", "ArmorTypeMetal", "ArmorTypeCombat"
        };
        static const char* const kMediumKws[] = {
            "sakr_kwd_pantsLong", "sakr_kwd_skirtLong", "sakr_kwd_pantsTagTight",
            "sakr_kwd_skirtTagTight", "sakr_kwd_armorBottomButt", "ArmorTypeMedium", "ArmorTypeLeather"
        };
        static const char* const kStandardKws[] = {
            "sakr_kwd_pantsShorts", "sakr_kwd_skirtShort", "sakr_kwd_pantyNormal",
            "ArmorTypeLight", "ArmorClothing"
        };
        static const char* const kSkimpyKws[] = {
            "sakr_kwd_pantsHotPants", "sakr_kwd_skirtMini", "sakr_kwd_skirtMicro",
            "sakr_kwd_pantsThong", "sakr_kwd_pantyTagSheer", "sakr_kwd_skirtTagSheer"
        };
        static const char* const kLewdKws[] = {
            "sakr_kwd_pantyThong", "sakr_kwd_pantyGString", "sakr_kwd_armorBottomLewd"
        };
        static const KeywordTierRule kRules[] = {
            { kHeavyKws,    sizeof(kHeavyKws)    / sizeof(kHeavyKws[0]),    { 0.35f, 2.00f, 1.40f, 1.0f } },
            { kMediumKws,   sizeof(kMediumKws)   / sizeof(kMediumKws[0]),   { 0.65f, 1.45f, 1.20f, 1.0f } },
            { kStandardKws, sizeof(kStandardKws) / sizeof(kStandardKws[0]), { 0.78f, 1.25f, 1.10f, 1.0f } },
            { kSkimpyKws,   sizeof(kSkimpyKws)   / sizeof(kSkimpyKws[0]),   { 0.90f, 1.10f, 1.05f, 1.0f } },
        };

        ApplyTierRules(kw, kRules, sizeof(kRules) / sizeof(kRules[0]),
            kLewdKws, sizeof(kLewdKws) / sizeof(kLewdKws[0]), 1.0f, state);
    }

    void InspectKeywordForm(BGSKeywordForm* kwForm, bool isBreastBone, bool isButtBone, KeywordTierState& state)
    {
        if (!kwForm || !kwForm->keywords || kwForm->numKeywords == 0)
            return;

        for (UInt32 k = 0; k < kwForm->numKeywords && !state.isPowerArmor; ++k)
        {
            BGSKeyword* kwd = kwForm->keywords[k];
            if (!kwd || !kwd->keyword.c_str())
                continue;

            const char* kw = kwd->keyword.c_str();
            if (isBreastBone)
                ClassifyBreastKeyword(kw, state);
            else if (isButtBone)
                ClassifyButtKeyword(kw, state);
        }
    }
}

actorUtils::ArmorSupportTier actorUtils::GetActorArmorSupport(Actor* actor, bool isBreastBone, bool isButtBone)
{
    ArmorSupportTier defaultSupport{ 1.0f, 1.0f, 1.0f, 1.0f };
    if (!actor || !IsActorValid(actor))
        return defaultSupport;

    // 1. Power Armor check: completely eliminates physics inside power armor
    if (IsActorInPowerArmor(actor))
    {
        return ArmorSupportTier{ 0.0f, 10.0f, 5.0f, 0.0f };
    }

    BipedAnim* biped = actor->biped.get();
    if (!biped)
        return defaultSupport;

    // Breast slots: 11 (Torso Armor 41), 3 (Body Outfit 33), 6 (Underwear Top 36), plus keyword-gated mod bra slots 25..28 (55..58).
    // Butt/Thigh slots: 3 (Body Outfit 33), 7 (Underwear Bottom 37), 10 (Left Leg 40), 12 (Right Leg 42).
    const UInt32 breastSlots[] = { 11, 3, 6, 25, 26, 27, 28 };
    const UInt32 buttSlots[] = { 3, 7, 10, 12 };

    const UInt32* slotsToCheck = isBreastBone ? breastSlots : (isButtBone ? buttSlots : nullptr);
    size_t numSlots = isBreastBone ? (sizeof(breastSlots) / sizeof(breastSlots[0])) :
                      (isButtBone ? (sizeof(buttSlots) / sizeof(buttSlots[0])) : 0);

    if (!slotsToCheck || numSlots == 0)
        return defaultSupport;

    // Determine actor's naked skin form if present so we don't treat bare skin as armor
    TESObjectARMO* raceSkin = actor->race ? actor->race->skinForm.skin : nullptr;
    TESObjectARMO* npcSkin = nullptr;
    if (actor->baseForm)
    {
        TESNPC* npc = DYNAMIC_CAST(actor->baseForm, TESForm, TESNPC);
        if (npc)
            npcSkin = npc->skinForm.skin;
    }

    const TESForm* inspectedForms[16];
    const TBO_InstanceData* inspectedInstances[16];
    size_t inspectedCount = 0;

    ArmorSupportTier aggregate{ 1.0f, 1.0f, 1.0f, 1.0f };
    bool foundAnyEquipped = false;

    for (size_t s = 0; s < numSlots; ++s)
    {
        UInt32 slotIdx = slotsToCheck[s];
        if (slotIdx >= BIPOBJECT::kEditorCount)
            continue;

        const BIPOBJECT& obj = biped->object[slotIdx];
        if (!obj.parent.object)
            continue;

        const TESForm* armorForm = (const TESForm*)obj.parent.object;
        if ((raceSkin && armorForm == raceSkin) || (npcSkin && armorForm == npcSkin))
            continue;

        const TBO_InstanceData* instData = obj.parent.instanceData;
        bool alreadyInspected = false;
        for (size_t i = 0; i < inspectedCount; ++i)
        {
            if (inspectedForms[i] == armorForm && inspectedInstances[i] == instData)
            {
                alreadyInspected = true;
                break;
            }
        }
        if (alreadyInspected)
            continue;

        if (inspectedCount < 16)
        {
            inspectedForms[inspectedCount] = armorForm;
            inspectedInstances[inspectedCount] = instData;
            ++inspectedCount;
        }

        KeywordTierState itemState;
        BGSKeywordForm* baseKeywords = DYNAMIC_CAST((TESForm*)armorForm, TESForm, BGSKeywordForm);
        InspectKeywordForm(baseKeywords, isBreastBone, isButtBone, itemState);

        // OMODs / armor modifications can supply or override keywords via InstanceData
        if (!itemState.isPowerArmor && instData)
        {
            auto* armInst = static_cast<TESObjectARMO::InstanceData*>(
                Runtime_DynamicCast((void*)instData, RTTI_TBO_InstanceData, RTTI_TESObjectARMO__InstanceData));
            if (armInst && armInst->keywords)
            {
                InspectKeywordForm(armInst->keywords, isBreastBone, isButtBone, itemState);
            }
        }

        // Extra mod slots (55..58) can hold non-garment accessories (backpacks, eyewear, etc.);
        // only count them as breast garments when they explicitly carry a relevant keyword.
        const bool isPrimarySlot = (slotIdx <= 12);
        if (!isPrimarySlot && !itemState.isPowerArmor && !itemState.hasSupportKeyword && !itemState.hasExplicitLewd)
            continue;

        foundAnyEquipped = true;

        if (itemState.isPowerArmor)
            return itemState.tier;

        if (!itemState.hasSupportKeyword)
        {
            if (itemState.hasExplicitLewd)
            {
                const ArmorSupportTier lewdTier = isBreastBone
                    ? ArmorSupportTier{ 0.95f, 1.02f, 1.01f, 0.92f }
                    : ArmorSupportTier{ 0.95f, 1.05f, 1.02f, 1.0f };
                ApplyTighterTier(itemState.tier, lewdTier);
            }
            else if (isPrimarySlot)
            {
                // Untagged clothing/armor defaults to at least standard bra / everyday garment support
                const ArmorSupportTier clothedDefault = isBreastBone
                    ? ArmorSupportTier{ 0.78f, 1.12f, 1.08f, 0.68f }
                    : ArmorSupportTier{ 0.78f, 1.25f, 1.10f, 1.0f };
                ApplyTighterTier(itemState.tier, clothedDefault);
            }
        }

        ApplyTighterTier(aggregate, itemState.tier);
    }

    if (!foundAnyEquipped)
        return defaultSupport;

    return aggregate;
}