/*
 * Copyright (C) 2008-2010 Trinity <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "AuctionHouseBot.h"
#include "AuctionHouseBotConfig.h"
#include "AuctionHouseMgr.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "WorldSession.h"
#include "GameTime.h"
#include "DatabaseEnv.h"
#include "Config.h"
#include "Player.h"
#include "AuctionHouseSearcher.h"

#include <algorithm>
#include <vector>
#include <set>
#include <random>
#include <sstream>
#include <map>

#include "AuctionHouseBotCommon.h"

using namespace std;
using CharacterDatabaseQueryHolder = SQLQueryHolder<CharacterDatabaseConnection>;

AuctionHouseSearcher* sAuctionHouseSearcher = nullptr;

AuctionHouseBot::AuctionHouseBot(uint32 account, uint32 id)
{
    _account        = account;
    _id             = id;

    _lastrun_a_sec  = time(NULL);
    _lastrun_h_sec  = time(NULL);
    _lastrun_n_sec  = time(NULL);

    _allianceConfig = NULL;
    _hordeConfig    = NULL;
    _neutralConfig  = NULL;
}

AuctionHouseBot::~AuctionHouseBot()
{
    // Nothing
}

uint32 AuctionHouseBot::getElement(std::set<uint32> set, int index, uint32 botId, uint32 maxDup, AuctionHouseObject* auctionHouse)
{
    std::set<uint32>::iterator it = set.begin();
    std::advance(it, index);

    if (maxDup > 0)
    {
        uint32 noStacks = 0;

        for (AuctionHouseObject::AuctionEntryMap::const_iterator itr = auctionHouse->GetAuctionsBegin(); itr != auctionHouse->GetAuctionsEnd(); ++itr)
        {
            AuctionEntry* Aentry = itr->second;

            if (Aentry->owner.GetCounter() == botId)
            {
                if (*it == Aentry->item_template)
                {
                    noStacks++;
                }
            }
        }

        if (noStacks >= maxDup)
        {
            return 0;
        }
    }

    return *it;
}

uint32 AuctionHouseBot::getStackCount(AHBConfig* config, uint32 max)
{
    uint32 maxStackSize = config->GetMaxStackSize();

    if (max == 1)
    {
        return 1;
    }

    //
    // Organize the stacks in a pseudo random way
    //

    if (config->DivisibleStacks)
    {
        uint32 ret = 0;

        if (max % 5 == 0) // 5, 10, 15, 20
        {
            ret = urand(1, 4) * 5;
        }

        if (max % 4 == 0) // 4, 8, 12, 16
        {
            ret = urand(1, 4) * 4;
        }

        if (max % 3 == 0) // 3, 6, 9, 18
        {
            ret = urand(1, 3) * 3;
        }

        if (ret > max)
        {
            ret = max;
        }

        return ret;
    }

    //
    // Totally random
    //

    return urand(1, std::min(max, maxStackSize));
}

uint32 AuctionHouseBot::getElapsedTime(uint32 timeClass)
{
    switch (timeClass)
    {
    case 2:
        return urand(1, 5) * 600;   // SHORT = In the range of one hour

    case 1:
        return urand(1, 23) * 3600; // MEDIUM = In the range of one day

    default:
        return urand(1, 3) * 86400; // LONG = More than one day but less than three
    }
}

uint32 AuctionHouseBot::getNofAuctions(AHBConfig* config, AuctionHouseObject* auctionHouse, ObjectGuid guid)
{
    // All the auctions
    if (!config->ConsiderOnlyBotAuctions)
    {
        return auctionHouse->Getcount();
    }

    // Just the one handled by the bot
    uint32 nofAuctions = 0;

    for (AuctionHouseObject::AuctionEntryMap::const_iterator itr = auctionHouse->GetAuctionsBegin(); itr != auctionHouse->GetAuctionsEnd(); ++itr)
    {
        AuctionEntry* Aentry = itr->second;

        if (guid == Aentry->owner)
        {
            nofAuctions++;
        }
    }

    return nofAuctions;
}

uint32 AuctionHouseBot::getTotalAuctions(AHBConfig* config, AuctionHouseObject* auctionHouse)
{
    uint32 totalAuctions = 0;
    for (uint32 guid : config->GetBotGUIDs())
    {
        totalAuctions += getNofAuctions(config, auctionHouse, ObjectGuid::Create<HighGuid::Player>(guid));
    }
    return totalAuctions;
}

// =============================================================================
// This routine performs the bidding operations for the bot
// =============================================================================

void AuctionHouseBot::Buy(Player* AHBplayer, AHBConfig* config, WorldSession* session)
{
    LOG_INFO("module", "AHBot [{}]: Starting buying process", _id);

    // Check if disabled
    if (!config->AHBBuyer)
    {
        LOG_INFO("module", "AHBot [{}]: Buyer is disabled", _id);
        return;
    }

    // Retrieve items not owned by the bot and not bought by the bot
    std::string botGUIDsStr = JoinGUIDs(config->GetBotGUIDs());
    uint32 auctionHouseID = config->GetAHID();
    LOG_INFO("module", "AHBot [{}]: Querying auction house {} for items not owned by bots", _id, auctionHouseID);
    QueryResult result = CharacterDatabase.Query("SELECT id FROM auctionhouse WHERE itemowner NOT IN ({}) AND buyguid NOT IN ({}) AND houseid = {}", botGUIDsStr, botGUIDsStr, auctionHouseID);

    if (!result || result->GetRowCount() == 0)
    {
        LOG_ERROR("module", "AHBot [{}]: No items found to buy in auction house {}. Bot GUIDs: {}", _id, auctionHouseID, botGUIDsStr);
        return;
    }

    // Fetches content of selected AH to look for possible bids
    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(config->GetAHFID());
    std::set<uint32> possibleBids;

    do
    {
        uint32 tmpdata = result->Fetch()->Get<uint32>();
        possibleBids.insert(tmpdata);
    } while (result->NextRow());

    LOG_INFO("module", "AHBot [{}]: Found {} possible bids", _id, possibleBids.size());

    // If it's not possible to bid stop here
    if (possibleBids.empty())
    {
        if (config->DebugOutBuyer)
        {
            LOG_INFO("module", "AHBot [{}]: no auctions to bid on has been recovered in auction house {}", _id, auctionHouseID);
        }

        return;
    }

    uint32 guid = AHBplayer->GetGUID().GetCounter();

    // Perform the operation for a maximum amount of bids attempts configured
    for (uint32 count = 1; count <= config->GetBidsPerInterval(); ++count)
    {
        LOG_INFO("module", "AHBot [{}]: Attempting bid {}/{}", _id, count, config->GetBidsPerInterval());

        // Choose a random auction from possible auctions
        uint32 randBid = urand(0, possibleBids.size() - 1);
        LOG_INFO("module", "AHBot [{}]: Random bid: {}", _id, randBid);

        std::set<uint32>::iterator it = possibleBids.begin();
        std::advance(it, randBid);

        AuctionEntry* auction = auctionHouse->GetAuction(*it);

        if (!auction)
        {
            LOG_ERROR("module", "AHBot [{}]: Could not find auction with ID {} in auction house {}", _id, *it, auctionHouseID);
            continue;
        }

        // Prevent from buying items from the other bots
        if (gBotsId.find(auction->owner.GetCounter()) != gBotsId.end())
        {
            LOG_INFO("module", "AHBot [{}]: Skipping auction owned by another bot", _id);
            continue;
        }

        // Get the item information
        Item* pItem = sAuctionMgr->GetAItem(auction->item_guid);

        if (!pItem)
        {
            if (config->DebugOutBuyer)
            {
                LOG_ERROR("module", "AHBot [{}]: item {} doesn't exist, perhaps bought already?", _id, auction->item_guid.ToString());
            }

            continue;
        }

        // Get the item prototype
        ItemTemplate const* prototype = sObjectMgr->GetItemTemplate(auction->item_template);
        if (!prototype)
        {
            LOG_ERROR("module", "AHBot [{}]: item {} has no prototype", _id, auction->item_guid.ToString());
            continue;
        }

        // Get price overrides
        auto [avgPrice, minPrice] = config->GetPriceOverrideForItem(prototype->ItemId);

        uint64 maxPrice = (avgPrice + ( avgPrice - minPrice ));
        uint64 SellPriceValue = maxPrice > 0 ? maxPrice : prototype->SellPrice;
        uint64 BuyPriceValue = avgPrice > 0 ? avgPrice : prototype->BuyPrice;

        // Calculate the bid and buyout prices
        uint32 currentprice = auction->bid ? auction->bid : auction->startbid;
        double bidrate = static_cast<double>(urand(1, 100)) / 100;
        long double bidMax = 0;

        LOG_INFO("module", "AHBot [{}]: Current price: {}", _id, currentprice);
        LOG_INFO("module", "AHBot [{}]: Bid rate: {}", _id, bidrate);

        // Check that bid has an acceptable value and take bid based on vendorprice, stacksize and quality
        // UseBuyPriceForBuyer = 1 -> Use Sell Price
        if (config->BuyMethod)
        {
            LOG_INFO("module", "AHBot [{}]: Buy method", _id);
            LOG_INFO("module", "AHBot [{}]: Quality: {}", _id, prototype->Quality);

            // Check if the quality is supported
            if (prototype->Quality <= AHB_MAX_QUALITY)
            {
                // Calculate the bid based on the quality
                // old calculation used quality multiplier from DB - this is not good and will be replaced by estimated max price based on min and avg price from price override, we still need a fallback if no proceoverride is given
                //if (currentprice < SellPriceToUse * pItem->GetCount() * config->GetBuyerPrice(prototype->Quality))

                uint64 maxPriceToUse = maxPrice > 0 ? (maxPrice * pItem->GetCount()) : (SellPriceValue * pItem->GetCount() * config->GetBuyerPrice(prototype->Quality));

                if (currentprice < maxPriceToUse)
                {
                    bidMax = maxPriceToUse;
                    //bidMax = SellPriceToUse * pItem->GetCount() * config->GetBuyerPrice(prototype->Quality);
                    LOG_INFO("module", "AHBot [{}]: SellPrice = {} pItem->GetCount = {} config->GetBuyerPrice(prototype->Quality) = {}", _id, maxPriceToUse,pItem->GetCount(),config->GetBuyerPrice(prototype->Quality));
                    LOG_INFO("module", "AHBot [{}]: Bid Max: {}", _id, bidMax);
                }
                else
                {
                    LOG_INFO("module", "AHBot [{}]: SellPrice = {} pItem->GetCount = {} config->GetBuyerPrice(prototype->Quality) = {}", _id, maxPriceToUse,pItem->GetCount(),config->GetBuyerPrice(prototype->Quality));
                    LOG_INFO("module", "AHBot [{}]: Current price {} is not less than calculated bid max {}", _id, currentprice, maxPriceToUse);
                }
            }
            else
            {
                if (config->DebugOutBuyer)
                {
                    LOG_ERROR("module", "AHBot [{}]: Quality {} not Supported", _id, prototype->Quality);
                }

                continue;
            }
        }
        // UseBuyPriceForBuyer = 0 -> Use Buy Price
        else
        {
            LOG_INFO("module", "AHBot [{}]: Bid method", _id);
            LOG_INFO("module", "AHBot [{}]: Quality: {}", _id, prototype->Quality);

            uint64 maxPriceToUse = avgPrice > 0 ? (avgPrice * pItem->GetCount()) : (BuyPriceValue * pItem->GetCount() * config->GetBuyerPrice(prototype->Quality));

            if (prototype->Quality <= AHB_MAX_QUALITY)
            {
                if (currentprice < maxPriceToUse)
                {
                    bidMax = maxPriceToUse;
                    LOG_INFO("module", "AHBot [{}]: bidMax = {}", _id, bidMax);
                }
                else
                {
                    LOG_INFO("module", "AHBot [{}]: Current price {} is not less than calculated bid max {}", _id, currentprice, maxPriceToUse);
                }
            }
            else
            {
                if (config->DebugOutBuyer)
                {
                    LOG_ERROR("module", "AHBot [{}]: Quality {} not Supported", _id, prototype->Quality);
                }

                continue;
            }
        }

        // Recalculate the bid depending on the type of the item
        switch (prototype->Class)
        {
            // ammo
        case 6:
            bidMax = 0;
            LOG_INFO("module", "AHBot [{}]: Skipping ammo item", _id);
            break;
        default:
            break;
        }

        // Test the computed bid
        if (bidMax == 0)
        {
            LOG_INFO("module", "AHBot [{}]: Computed bidMax is 0, skipping", _id);
            continue;
        }

        // Calculate our bid
        long double bidvalue = currentprice + ((bidMax - currentprice) * bidrate);
        uint32      bidprice = static_cast<uint32>(bidvalue);

        // Check our bid is high enough to be valid. If not, correct it to minimum.
        if ((currentprice + auction->GetAuctionOutBid()) > bidprice)
        {
            bidprice = currentprice + auction->GetAuctionOutBid();
            LOG_INFO("module", "AHBot [{}]: Bid price too low, corrected to minimum", _id);
        }

        // Print out debug info
        if (config->DebugOutBuyer)
        {
            LOG_INFO("module", "-------------------------------------------------");
            LOG_INFO("module", "AHBot [{}]: Info for Auction #{}:", _id, auction->Id);
            LOG_INFO("module", "AHBot [{}]: AuctionHouse: {}"     , _id, auction->GetHouseId());
            LOG_INFO("module", "AHBot [{}]: Owner: {}"            , _id, auction->owner.ToString());
            LOG_INFO("module", "AHBot [{}]: Bidder: {}"           , _id, auction->bidder.ToString());
            LOG_INFO("module", "AHBot [{}]: Starting Bid: {}"     , _id, auction->startbid);
            LOG_INFO("module", "AHBot [{}]: Current Bid: {}"      , _id, currentprice);
            LOG_INFO("module", "AHBot [{}]: Buyout: {}"           , _id, auction->buyout);
            LOG_INFO("module", "AHBot [{}]: Deposit: {}"          , _id, auction->deposit);
            LOG_INFO("module", "AHBot [{}]: Expire Time: {}"      , _id, uint32(auction->expire_time));
            LOG_INFO("module", "AHBot [{}]: Bid Rate: {}"         , _id, bidrate);
            LOG_INFO("module", "AHBot [{}]: Bid Max: {}"          , _id, bidMax);
            LOG_INFO("module", "AHBot [{}]: Bid Value: {}"        , _id, bidvalue);
            LOG_INFO("module", "AHBot [{}]: Bid Price: {}"        , _id, bidprice);
            LOG_INFO("module", "AHBot [{}]: Item GUID: {}"        , _id, auction->item_guid.ToString());
            LOG_INFO("module", "AHBot [{}]: Item Template: {}"    , _id, auction->item_template);
            LOG_INFO("module", "AHBot [{}]: Item Info:"           , _id);
            LOG_INFO("module", "AHBot [{}]: Item ID: {}"          , _id, prototype->ItemId);
            LOG_INFO("module", "AHBot [{}]: Buy Price: {}"        , _id, prototype->BuyPrice);
            LOG_INFO("module", "AHBot [{}]: Sell Price: {}"       , _id, prototype->SellPrice);
            LOG_INFO("module", "AHBot [{}]: Bonding: {}"          , _id, prototype->Bonding);
            LOG_INFO("module", "AHBot [{}]: Quality: {}"          , _id, prototype->Quality);
            LOG_INFO("module", "AHBot [{}]: Item Level: {}"       , _id, prototype->ItemLevel);
            LOG_INFO("module", "AHBot [{}]: Ammo Type: {}"        , _id, prototype->AmmoType);
            LOG_INFO("module", "-------------------------------------------------");
        }

        // Check whether we do normal bid, or buyout
        bool bought = false;

        if ((bidprice < auction->buyout) || (auction->buyout == 0))
        {
            // Perform a new bid on the auction
            if (auction->bidder)
            {
                if (auction->bidder != AHBplayer->GetGUID())
                {
                    // Mail to last bidder and return their money
                    auto trans = CharacterDatabase.BeginTransaction();
                    sAuctionMgr->SendAuctionOutbiddedMail(auction, bidprice, session->GetPlayer(), trans);
                    CharacterDatabase.CommitTransaction(trans);
                }
            }

            auction->bidder = AHBplayer->GetGUID();
            auction->bid    = bidprice;

            // Save the auction into database
            CharacterDatabase.Execute("UPDATE auctionhouse SET buyguid = '{}', lastbid = '{}' WHERE id = '{}'", auction->bidder.GetCounter(), auction->bid, auction->Id);

            // Notify the auction house of the bid update
            sAuctionMgr->GetAuctionHouseSearcher()->UpdateBid(auction);
            //sAuctionHouseSearcher->UpdateBid(auction);

            LOG_INFO("module", "AHBot [{}]: Placed bid on auction ID {} with bid price {}", _id, auction->Id, bidprice);
        }
        else
        {
            bought = true;

            // Perform the buyout
            auto trans = CharacterDatabase.BeginTransaction();

            if ((auction->bidder) && (AHBplayer->GetGUID() != auction->bidder))
            {
                // Send the mail to the last bidder
                sAuctionMgr->SendAuctionOutbiddedMail(auction, auction->buyout, session->GetPlayer(), trans);
            }

            auction->bidder = AHBplayer->GetGUID();
            auction->bid    = auction->buyout;

            // Send mails to buyer & seller
            sAuctionMgr->SendAuctionSuccessfulMail(auction, trans);
            sAuctionMgr->SendAuctionWonMail(auction, trans);

            // Removes any trace of the item
            auction->DeleteFromDB(trans);

            sAuctionMgr->RemoveAItem(auction->item_guid);
            auctionHouse->RemoveAuction(auction);

            CharacterDatabase.CommitTransaction(trans);
            LOG_INFO("module", "AHBot [{}]: Bought out auction ID {} with buyout price {}", _id, auction->Id, auction->buyout);
        }

        // Tracing
        if (config->TraceBuyer)
        {
            if (bought)
            {
                LOG_INFO("module", "AHBot [{}]: Bought , id={}, ah={}, item={}, start={}, current={}, buyout={}", _id, prototype->ItemId, auction->GetHouseId(), auction->item_template, auction->startbid, currentprice, auction->buyout);
            }
            else
            {
                LOG_INFO("module", "AHBot [{}]: New bid, id={}, ah={}, item={}, start={}, current={}, buyout={}", _id, prototype->ItemId, auction->GetHouseId(), auction->item_template, auction->startbid, currentprice, auction->buyout);
            }
        }

        // Prevent to bid again on the same auction
        possibleBids.erase(it);
    }

    LOG_INFO("module", "AHBot [{}]: Completed buying process", _id);
}

// =============================================================================
// This routine performs the selling operations for the bot
// =============================================================================

void AuctionHouseBot::Sell(Player* AHBplayer, AHBConfig* config)
{
    // Check if disabled
    if (!config->AHBSeller)
    {
        LOG_INFO("module", "AHBot [{}]: Seller is disabled", _id);
        return;
    }

    // Retrieve the auction house situation
    AuctionHouseEntry const* ahEntry = sAuctionMgr->GetAuctionHouseEntryFromFactionTemplate(config->GetAHFID());
    if (!ahEntry)
    {
        LOG_ERROR("module", "AHBot [{}]: Could not retrieve auction house entry", _id);
        return;
    }

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(config->GetAHFID());
    if (!auctionHouse)
    {
        LOG_ERROR("module", "AHBot [{}]: Could not retrieve auction house object", _id);
        return;
    }

    auctionHouse->Update();

    // Calculate total auctions across all characters
    uint32 totalAuctions = getTotalAuctions(config, auctionHouse);
    uint32 minItems = config->GetMinItems();
    uint32 maxItems = config->GetMaxItems();

    if (maxItems == 0 || totalAuctions >= maxItems)
    {
        if (config->DebugOutSeller)
        {
            LOG_ERROR("module", "AHBot [{}]: Total auctions at or above maximum", _id);
        }
        return;
    }

     // Divide maxItems by the number of bots to get the max items per bot
    uint32 numBots = config->GetBotGUIDs().size();
    uint32 maxAuctionsPerBot = (maxItems + numBots - 1) / numBots;
    uint32 minAuctionsPerBot = (minItems + numBots - 1) / numBots;

    uint32 auctions = getNofAuctions(config, auctionHouse, AHBplayer->GetGUID());
    uint32 items = 0;

    bool aboveMin = false;
    bool aboveMax = false;

    if (auctions >= maxAuctionsPerBot)
    {
        aboveMax = true;
        if (config->DebugOutSeller)
        {
            std::string guidStr = AHBplayer->GetGUID().ToString();
            LOG_ERROR("module", "AHBot [{}]: Auctions above maximum for bot {}", _id, guidStr);
        }
        return;
    }

    // Determine the number of items to list
     if (totalAuctions < minItems)
    {
        // Add new auctions quickly until totalAuctions reach minItems
        uint32 maxItemsToList = minAuctionsPerBot - auctions;

        // Ensure maxItemsToList is greater than or equal to minAuctionsPerBot
        if (maxItemsToList < minAuctionsPerBot)
        {
            maxItemsToList = minAuctionsPerBot;
        }

        items = urand(maxItemsToList, minAuctionsPerBot);
    }
    else
    {
        // Gradually increase the number of auctions with ItemsPerCycle
        uint32 maxItemsToList = maxAuctionsPerBot - auctions;
        items = config->ItemsPerCycle;

        if (items > maxItemsToList)
        {
            items = maxItemsToList;
        }
    }

    // Use the max stack size configuration value
    uint32 maxStackSize = config->GetMaxStackSize();

    // Retrieve the configuration for this run
    uint32 greyTGcount   = config->GetMaximum(AHB_GREY_TG);
    uint32 whiteTGcount  = config->GetMaximum(AHB_WHITE_TG);
    uint32 greenTGcount  = config->GetMaximum(AHB_GREEN_TG);
    uint32 blueTGcount   = config->GetMaximum(AHB_BLUE_TG);
    uint32 purpleTGcount = config->GetMaximum(AHB_PURPLE_TG);
    uint32 orangeTGcount = config->GetMaximum(AHB_ORANGE_TG);
    uint32 yellowTGcount = config->GetMaximum(AHB_YELLOW_TG);

    uint32 greyIcount    = config->GetMaximum(AHB_GREY_I);
    uint32 whiteIcount   = config->GetMaximum(AHB_WHITE_I);
    uint32 greenIcount   = config->GetMaximum(AHB_GREEN_I);
    uint32 blueIcount    = config->GetMaximum(AHB_BLUE_I);
    uint32 purpleIcount  = config->GetMaximum(AHB_PURPLE_I);
    uint32 orangeIcount  = config->GetMaximum(AHB_ORANGE_I);
    uint32 yellowIcount  = config->GetMaximum(AHB_YELLOW_I);

    uint32 greyTGoods    = config->GetItemCounts(AHB_GREY_TG);
    uint32 whiteTGoods   = config->GetItemCounts(AHB_WHITE_TG);
    uint32 greenTGoods   = config->GetItemCounts(AHB_GREEN_TG);
    uint32 blueTGoods    = config->GetItemCounts(AHB_BLUE_TG);
    uint32 purpleTGoods  = config->GetItemCounts(AHB_PURPLE_TG);
    uint32 orangeTGoods  = config->GetItemCounts(AHB_ORANGE_TG);
    uint32 yellowTGoods  = config->GetItemCounts(AHB_YELLOW_TG);

    uint32 greyItems     = config->GetItemCounts(AHB_GREY_I);
    uint32 whiteItems    = config->GetItemCounts(AHB_WHITE_I);
    uint32 greenItems    = config->GetItemCounts(AHB_GREEN_I);
    uint32 blueItems     = config->GetItemCounts(AHB_BLUE_I);
    uint32 purpleItems   = config->GetItemCounts(AHB_PURPLE_I);
    uint32 orangeItems   = config->GetItemCounts(AHB_ORANGE_I);
    uint32 yellowItems   = config->GetItemCounts(AHB_YELLOW_I);

    // Initialize item counts
    std::vector<uint32> itemCounts(14, 0);

    // Get prioritized item IDs
    std::vector<uint32> itemsToSell = GetItemsToSell(config, AHBplayer->GetGUID());

    // Loop variables
    uint32 noSold    = 0; // Tracing counter
    uint32 binEmpty  = 0; // Tracing counter
    uint32 noNeed    = 0; // Tracing counter
    uint32 tooMany   = 0; // Tracing counter
    uint32 loopBrk   = 0; // Tracing counter
    uint32 err       = 0; // Tracing counter

    //for (uint32 cnt = 1; cnt <= items; cnt++)
    for (uint32 itemID : itemsToSell)
    {
        if (auctions >= maxAuctionsPerBot)
        {
            break;
        }

        // Select item by rarity
        uint32 choice = 0;
        uint32 itemID = 0;
        uint32 loopbreaker = 0;

        while (itemID == 0 && loopbreaker <= AUCTION_HOUSE_BOT_LOOP_BREAKER)
        {
            loopbreaker++;

            // Poor
            if ((config->GreyItemsBin.size() > 0) && (greyItems < config->GetMaximum(AHB_GREY_I)))
            {
                itemID = getElement(config->GreyItemsBin, urand(0, config->GreyItemsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
                choice = 0;
                greyItems++;
            }

            if (itemID == 0 && (config->GreyTradeGoodsBin.size() > 0) && (greyTGoods < config->GetMaximum(AHB_GREY_TG)))
            {
                itemID = getElement(config->GreyTradeGoodsBin, urand(0, config->GreyTradeGoodsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
                choice = 7;
                greyTGoods++;
            }

            // Normal
            if (itemID == 0 && (config->WhiteItemsBin.size() > 0) && (whiteItems < config->GetMaximum(AHB_WHITE_I)))
            {
                itemID = getElement(config->WhiteItemsBin, urand(0, config->WhiteItemsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
                choice = 1;
                whiteItems++;
            }

            if (itemID == 0 && (config->WhiteTradeGoodsBin.size() > 0) && (whiteTGoods < config->GetMaximum(AHB_WHITE_TG)))
            {
                itemID = getElement(config->WhiteTradeGoodsBin, urand(0, config->WhiteTradeGoodsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
                choice = 8;
                whiteTGoods++;
            }

            // Uncommon
            if (itemID == 0 && (config->GreenItemsBin.size() > 0) && (greenItems < config->GetMaximum(AHB_GREEN_I)))
            {
                itemID = getElement(config->GreenItemsBin, urand(0, config->GreenItemsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
                choice = 2;
                greenItems++;
            }

            if (itemID == 0 && (config->GreenTradeGoodsBin.size() > 0) && (greenTGoods < config->GetMaximum(AHB_GREEN_TG)))
            {
                itemID = getElement(config->GreenTradeGoodsBin, urand(0, config->GreenTradeGoodsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
                choice = 9;
                greenTGoods++;
            }

            // Rare
            if (itemID == 0 && (config->BlueItemsBin.size() > 0) && (blueItems < config->GetMaximum(AHB_BLUE_I)))
            {
                itemID = getElement(config->BlueItemsBin, urand(0, config->BlueItemsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
                choice = 3;
                blueItems++;
            }

            if (itemID == 0 && (config->BlueTradeGoodsBin.size() > 0) && (blueTGoods < config->GetMaximum(AHB_BLUE_TG)))
            {
                itemID = getElement(config->BlueTradeGoodsBin, urand(0, config->BlueTradeGoodsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
                choice = 10;
                blueTGoods++;
            }

            // Epic
            if (itemID == 0 && (config->PurpleItemsBin.size() > 0) && (purpleItems < config->GetMaximum(AHB_PURPLE_I)))
            {
                itemID = getElement(config->PurpleItemsBin, urand(0, config->PurpleItemsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
                choice = 4;
                purpleItems++;
            }

            if (itemID == 0 && (config->PurpleTradeGoodsBin.size() > 0) && (purpleTGoods < config->GetMaximum(AHB_PURPLE_TG)))
            {
                itemID = getElement(config->PurpleTradeGoodsBin, urand(0, config->PurpleTradeGoodsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
                choice = 11;
                purpleTGoods++;
            }

            // Legendary
            if (itemID == 0 && (config->OrangeItemsBin.size() > 0) && (orangeItems < config->GetMaximum(AHB_ORANGE_I)))
            {
                itemID = getElement(config->OrangeItemsBin, urand(0, config->OrangeItemsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
                choice = 5;
                orangeItems++;
            }

            if (itemID == 0 && (config->OrangeTradeGoodsBin.size() > 0) && (orangeTGoods < config->GetMaximum(AHB_ORANGE_TG)))
            {
                itemID = getElement(config->OrangeTradeGoodsBin, urand(0, config->OrangeTradeGoodsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
                choice = 12;
                orangeTGoods++;
            }

            // Artifact
            if (itemID == 0 && (config->YellowItemsBin.size() > 0) && (yellowItems < config->GetMaximum(AHB_YELLOW_I)))
            {
                itemID = getElement(config->YellowItemsBin, urand(0, config->YellowItemsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
                choice = 6;
                yellowItems++;
            }

            if (itemID == 0 && (config->YellowTradeGoodsBin.size() > 0) && (yellowTGoods < config->GetMaximum(AHB_YELLOW_TG)))
            {
                itemID = getElement(config->YellowTradeGoodsBin, urand(0, config->YellowTradeGoodsBin.size() - 1), _id, config->DuplicatesCount, auctionHouse);
                choice = 13;
                yellowTGoods++;

            }

            if (itemID == 0)
            {
                if (config->DebugOutSeller)
                {
                    LOG_ERROR("module", "AHBot [{}]: No item could be selected from the bins", _id);
                }

                break;
            }
        }

        if (itemID == 0)
        {
            loopBrk++;
            return;
        }

        // Retrieve information about the selected item
        ItemTemplate const* prototype = sObjectMgr->GetItemTemplate(itemID);

        if (prototype == NULL)
        {
            err++;

            if (config->DebugOutSeller)
            {
                LOG_ERROR("module", "AHBot [{}]: could not get prototype of item {}", _id, itemID);
            }

            return;
        }

        Item* item = Item::CreateItem(itemID, 1, AHBplayer);

        if (item == NULL)
        {
            err++;

            if (config->DebugOutSeller)
            {
                LOG_ERROR("module", "AHBot [{}]: could not create item from prototype {}", _id, itemID);
            }

            return;
        }

        // Start interacting with the item by adding a random property
        item->AddToUpdateQueueOf(AHBplayer);

        uint32 randomPropertyId = Item::GenerateItemRandomPropertyId(itemID);

        if (randomPropertyId != 0)
        {
            item->SetItemRandomProperties(randomPropertyId);
        }

        if (prototype->Quality > AHB_MAX_QUALITY)
        {
            err++;

            if (config->DebugOutSeller)
            {
                LOG_ERROR("module", "AHBot [{}]: Quality {} TOO HIGH for item {}", _id, prototype->Quality, itemID);
            }

            item->RemoveFromUpdateQueueOf(AHBplayer);
            return;
        }

        // Determine the price
        uint64 buyoutPrice = 0;
        uint64 bidPrice    = 0;
        uint32 stackCount  = 1;
        uint64 baseBuyoutPrice = 0;
        uint64 baseBidPrice = 0;

        auto it = config->itemPriceOverrides.find(itemID);
        if (it != config->itemPriceOverrides.end())
        {
            // Base prices are loaded from from the price override table in the database
            baseBuyoutPrice = std::get<0>(it->second);
            baseBidPrice = std::get<1>(it->second);

        }
        else
        {
            // No price override was found, fall back to fixed default prices
            if (config->SellAtMarketPrice)
            {
                baseBuyoutPrice = config->GetItemPrice(itemID);
            }

            if (baseBuyoutPrice == 0)
            {
                if (config->SellMethod)
                {
                    baseBuyoutPrice = prototype->BuyPrice;
                }
                else
                {
                    baseBuyoutPrice = prototype->SellPrice;
                }
            }

            baseBuyoutPrice = baseBuyoutPrice * urand(config->GetMinPrice(prototype->Quality), config->GetMaxPrice(prototype->Quality));
            baseBuyoutPrice = baseBuyoutPrice / 100;

            baseBidPrice    = baseBuyoutPrice * urand(config->GetMinBidPrice(prototype->Quality), config->GetMaxBidPrice(prototype->Quality));
            baseBidPrice    = baseBidPrice / 100;
        }

        // Introduce randomness around the baseline values with a range of -10% to +10%
        int32 buyoutDeviation = urand(-10, 10);
        int32 bidDeviation = urand(-10, 10);

        // Adjust the baseline values with random deviation
        buyoutPrice = baseBuyoutPrice * (1 + buyoutDeviation / 100.0);
        bidPrice = baseBidPrice * (1 + bidDeviation / 100.0);


        // Determine the stack size
        if (config->GetMaxStack(prototype->Quality) > 1 && item->GetMaxStackCount() > 1)
        {
            stackCount = minValue(getStackCount(config, item->GetMaxStackCount()), config->GetMaxStack(prototype->Quality));
        }
        else if (config->GetMaxStack(prototype->Quality) == 0 && item->GetMaxStackCount() > 1)
        {
            stackCount = getStackCount(config, item->GetMaxStackCount());
        }
        else
        {
            stackCount = 1;
        }

        item->SetCount(stackCount);

        // Determine the auction time
        uint32 etime = getElapsedTime(config->ElapsingTimeClass);

        // Determine the deposit
        uint32 dep   = sAuctionMgr->GetAuctionDeposit(ahEntry, etime, item, stackCount);

        // Perform the auction
        auto trans = CharacterDatabase.BeginTransaction();

        AuctionEntry* auctionEntry      = new AuctionEntry();
        auctionEntry->Id                = sObjectMgr->GenerateAuctionID();
        auctionEntry->houseId           = AuctionHouseId(config->GetAHID());
        auctionEntry->item_guid         = item->GetGUID();
        auctionEntry->item_template     = item->GetEntry();
        auctionEntry->itemCount         = item->GetCount();
        auctionEntry->owner             = AHBplayer->GetGUID();
        auctionEntry->startbid          = bidPrice * stackCount;
        auctionEntry->buyout            = buyoutPrice * stackCount;
        auctionEntry->bid               = 0;
        auctionEntry->deposit           = dep;
        auctionEntry->expire_time       = (time_t)etime + time(NULL);
        auctionEntry->auctionHouseEntry = ahEntry;

        item->SaveToDB(trans);
        item->RemoveFromUpdateQueueOf(AHBplayer);
        sAuctionMgr->AddAItem(item);
        auctionHouse->AddAuction(auctionEntry);
        auctionEntry->SaveToDB(trans);

        CharacterDatabase.CommitTransaction(trans);

        // Increments the number of items presents in the auction
        switch (choice)
        {
        case 0:
            ++greyItems;
            break;

        case 1:
            ++whiteItems;
            break;

        case 2:
            ++greenItems;
            break;

        case 3:
            ++blueItems;
            break;

        case 4:
            ++purpleItems;
            break;

        case 5:
            ++orangeItems;
            break;

        case 6:
            ++yellowItems;
            break;

        case 7:
            ++greyTGoods;
            break;

        case 8:
            ++whiteTGoods;
            break;

        case 9:
            ++greenTGoods;
            break;

        case 10:
            ++blueTGoods;
            break;

        case 11:
            ++purpleTGoods;
            break;

        case 12:
            ++orangeTGoods;
            break;

        case 13:
            ++yellowTGoods;
            break;

        default:
            break;
        }

        noSold++;

        if (config->TraceSeller)
        {
            //TODO: add item name, quality and stack size as well as totalAuctions
            LOG_INFO("module", "AHBot [{}]: New stack ah={}, id={}, stack={}, bid={}, buyout={}", _id, config->GetAHID(), itemID, stackCount, auctionEntry->startbid, auctionEntry->buyout);
        }
    }

    if (config->TraceSeller)
    {
        LOG_INFO("module", "AHBot [{}]: auctionhouse {}, req={}, sold={}, aboveMin={}, aboveMax={}, loopBrk={}, noNeed={}, tooMany={}, binEmpty={}, err={}", _id, config->GetAHID(), items, noSold, aboveMin, aboveMax, loopBrk, noNeed, tooMany, binEmpty, err);
    }

    totalAuctions += items;
}

// =============================================================================
// Get Prioritized ItemIDs
// =============================================================================

std::vector<uint32> AuctionHouseBot::GetItemsToSell(AHBConfig* config, ObjectGuid botGuid)
{
    std::vector<uint32> prioritizedItemIDs;

    // Prioritize items with price overrides that are not listed by the bot yet
    for (const auto& [itemID, prices] : config->itemPriceOverrides)
    {
        if (!IsItemListedByBot(itemID, config->GetAHID(), botGuid))
        {
            prioritizedItemIDs.push_back(itemID);
        }
    }

    // If there are no such items, pick items for which price overrides do exist
    if (prioritizedItemIDs.empty())
    {
        for (const auto& [itemID, prices] : config->itemPriceOverrides)
        {
            prioritizedItemIDs.push_back(itemID);
        }
    }

    // If there are still no items, fall back to items without overrides that are not in the auction house
    if (prioritizedItemIDs.empty())
    {
        for (uint32 itemID : GetAllItemIDs(config->GetAHID()))
        {
            if (config->itemPriceOverrides.find(itemID) == config->itemPriceOverrides.end() && !IsItemInAuctionHouse(itemID, config->GetAHID()))
            {
                prioritizedItemIDs.push_back(itemID);
            }
        }
    }

    // If there are still no items, fall back to random items without price overrides
    if (prioritizedItemIDs.empty())
    {
        for (uint32 itemID : GetAllItemIDs(config->GetAHID()))
        {
            if (config->itemPriceOverrides.find(itemID) == config->itemPriceOverrides.end())
            {
                prioritizedItemIDs.push_back(itemID);
            }
        }
    }

    // Shuffle the prioritized item IDs to add randomness
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(prioritizedItemIDs.begin(), prioritizedItemIDs.end(), g);

    return prioritizedItemIDs;
}

// =============================================================================
// Find out if item is listed by current bot
// =============================================================================

bool AuctionHouseBot::IsItemListedByBot(uint32 itemID, uint32 ahID, ObjectGuid botGuid)
{
    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(ahID);
    if (!auctionHouse)
    {
        return false;
    }

    const std::map<uint32, AuctionEntry*>& auctions = auctionHouse->GetAuctions();
    for (const auto& auction : auctions)
    {
        if (auction.second->item_template == itemID && auction.second->owner == botGuid)
        {
            return true;
        }
    }
    return false;
}

// =============================================================================
// Find out if item is listed in the aution house
// =============================================================================

bool AuctionHouseBot::IsItemInAuctionHouse(uint32 itemID, uint32 ahID)
{
    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(ahID);
    if (!auctionHouse)
    {
        return false;
    }

    const std::map<uint32, AuctionEntry*>& auctions = auctionHouse->GetAuctions();
    for (const auto& auction : auctions)
    {
        if (auction.second->item_template == itemID)
        {
            return true;
        }
    }
    return false;
}

// =============================================================================
// GetAllItemIDs that are not disabled and can be listed in the auctionhouse
// =============================================================================

std::vector<uint32> AuctionHouseBot::GetAllItemIDs(uint32 ahID)
{
    std::vector<uint32> allItemIDs;
    std::set<uint32> disabledItems;

    // Retrieve the list of disabled items from the database
    QueryResult result = WorldDatabase.Query("SELECT item FROM mod_auctionhousebot_disabled_items");

    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            disabledItems.insert(fields[0].Get<uint32>());
        } while (result->NextRow());
    }

    // Retrieve all item templates
    ItemTemplateContainer const* its = sObjectMgr->GetItemTemplateStore();

    for (ItemTemplateContainer::const_iterator itr = its->begin(); itr != its->end(); ++itr)
    {
        uint32 itemID = itr->second.ItemId;

        // Check if the item is not in the disabled items list
        if (disabledItems.find(itemID) == disabledItems.end() && !IsItemInAuctionHouse(itemID, ahID))
        {
            allItemIDs.push_back(itemID);
        }
    }

    return allItemIDs;
}

// =============================================================================
// Perform an update cycle
// =============================================================================

void AuctionHouseBot::Update()
{
    time_t _newrun = time(NULL);

    // If no configuration is associated, then stop here
    if (!_allianceConfig && !_hordeConfig && !_neutralConfig)
    {
        return;
    }

    // Preprare for operation
    std::string accountName = "AuctionHouseBot" + std::to_string(_account);

    WorldSession _session(_account, std::move(accountName), nullptr, SEC_PLAYER, sWorld->getIntConfig(CONFIG_EXPANSION), 0, LOCALE_enUS, 0, false, false, 0);

    Player _AHBplayer(&_session);
    _AHBplayer.Initialize(_id);

    ObjectAccessor::AddObject(&_AHBplayer);

    // Perform update for the factions markets
    if (!sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION))
    {
        // Alliance
        if (_allianceConfig)
        {
            Sell(&_AHBplayer, _allianceConfig);

            if (((_newrun - _lastrun_a_sec) >= (_allianceConfig->GetBiddingInterval() * MINUTE)) && (_allianceConfig->GetBidsPerInterval() > 0))
            {
                Buy(&_AHBplayer, _allianceConfig, &_session);
                _lastrun_a_sec = _newrun;
            }
        }

        // Horde
        if (_hordeConfig)
        {
            Sell(&_AHBplayer, _hordeConfig);

            if (((_newrun - _lastrun_h_sec) >= (_hordeConfig->GetBiddingInterval() * MINUTE)) && (_hordeConfig->GetBidsPerInterval() > 0))
            {
                Buy(&_AHBplayer, _hordeConfig, &_session);
                _lastrun_h_sec = _newrun;
            }
        }

    }
    else
    {
        //TODO: Implement two-side interaction
        // Handle the case where two-side interaction is allowed
        // Similar logic for alliance, horde, and neutral configurations
    }

    // Neutral
    if (_neutralConfig)
    {
        Sell(&_AHBplayer, _neutralConfig);

        if (((_newrun - _lastrun_n_sec) >= (_neutralConfig->GetBiddingInterval() * MINUTE)) && (_neutralConfig->GetBidsPerInterval() > 0))
        {
            Buy(&_AHBplayer, _neutralConfig, &_session);
            _lastrun_n_sec = _newrun;
        }
    }

    ObjectAccessor::RemoveObject(&_AHBplayer);
}

// =============================================================================
// Execute commands coming from the console
// =============================================================================

void AuctionHouseBot::Commands(AHBotCommand command, uint32 ahMapID, uint32 col, char* args)
{
    //
    // Retrieve the auction house configuration
    //

    AHBConfig *config = NULL;

    switch (ahMapID)
    {
    case 2:
        config = _allianceConfig;
        break;
    case 6:
        config = _hordeConfig;
        break;
    default:
        config = _neutralConfig;
        break;
    }

    //
    // Retrive the item quality
    //

    std::string color;

    switch (col)
    {
    case AHB_GREY:
        color = "grey";
        break;
    case AHB_WHITE:
        color = "white";
        break;
    case AHB_GREEN:
        color = "green";
        break;
    case AHB_BLUE:
        color = "blue";
        break;
    case AHB_PURPLE:
        color = "purple";
        break;
    case AHB_ORANGE:
        color = "orange";
        break;
    case AHB_YELLOW:
        color = "yellow";
        break;
    default:
        break;
    }

    //
    // Perform the command
    //

    switch (command)
    {
    case AHBotCommand::buyer:
    {
        char* param1 = strtok(args, " ");
        uint32 state = (uint32)strtoul(param1, NULL, 0);

        if (state == 0)
        {
            _allianceConfig->AHBBuyer = false;
            _hordeConfig->AHBBuyer    = false;
            _neutralConfig->AHBBuyer  = false;
        }
        else
        {
            _allianceConfig->AHBBuyer = true;
            _hordeConfig->AHBBuyer    = true;
            _neutralConfig->AHBBuyer  = true;
        }

        break;
    }
    case AHBotCommand::seller:
    {
        char* param1 = strtok(args, " ");
        uint32 state = (uint32)strtoul(param1, NULL, 0);

        if (state == 0)
        {
            _allianceConfig->AHBSeller = false;
            _hordeConfig->AHBSeller    = false;
            _neutralConfig->AHBSeller  = false;
        }
        else
        {
            _allianceConfig->AHBSeller = true;
            _hordeConfig->AHBSeller    = true;
            _neutralConfig->AHBSeller  = true;
        }

        break;
    }
    case AHBotCommand::useMarketPrice:
    {
        char* param1 = strtok(args, " ");
        uint32 state = (uint32)strtoul(param1, NULL, 0);

        if (state == 0)
        {
            _allianceConfig->SellAtMarketPrice = false;
            _hordeConfig->SellAtMarketPrice    = false;
            _neutralConfig->SellAtMarketPrice  = false;
        }
        else
        {
            _allianceConfig->SellAtMarketPrice = true;
            _hordeConfig->SellAtMarketPrice    = true;
            _neutralConfig->SellAtMarketPrice  = true;
        }

        break;
    }
    case AHBotCommand::ahexpire:
    {
        AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(config->GetAHFID());

        AuctionHouseObject::AuctionEntryMap::iterator itr;
        itr = auctionHouse->GetAuctionsBegin();

        //
        // Iterate through all the autions and if they belong to the bot, make them expired
        //

        while (itr != auctionHouse->GetAuctionsEnd())
        {
            if (itr->second->owner.GetCounter() == _id)
            {
                // Expired NOW.
                itr->second->expire_time = GameTime::GetGameTime().count();

                uint32 id                = itr->second->Id;
                uint32 expire_time       = itr->second->expire_time;

                CharacterDatabase.Execute("UPDATE auctionhouse SET time = '{}' WHERE id = '{}'", expire_time, id);
            }

            ++itr;
        }

        break;
    }
    case AHBotCommand::minitems:
    {
        char * param1   = strtok(args, " ");
        uint32 minItems = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET minitems = '{}' WHERE auctionhouse = '{}'", minItems, ahMapID);

        config->SetMinItems(minItems);

        break;
    }
    case AHBotCommand::maxitems:
    {
        char * param1   = strtok(args, " ");
        uint32 maxItems = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET maxitems = '{}' WHERE auctionhouse = '{}'", maxItems, ahMapID);

        config->SetMaxItems(maxItems);
        config->CalculatePercents();
        break;
    }
    case AHBotCommand::percentages:
    {
        char * param1   = strtok(args, " ");
        char * param2   = strtok(NULL, " ");
        char * param3   = strtok(NULL, " ");
        char * param4   = strtok(NULL, " ");
        char * param5   = strtok(NULL, " ");
        char * param6   = strtok(NULL, " ");
        char * param7   = strtok(NULL, " ");
        char * param8   = strtok(NULL, " ");
        char * param9   = strtok(NULL, " ");
        char * param10  = strtok(NULL, " ");
        char * param11  = strtok(NULL, " ");
        char * param12  = strtok(NULL, " ");
        char * param13  = strtok(NULL, " ");
        char * param14  = strtok(NULL, " ");

        uint32 greytg   = (uint32) strtoul(param1, NULL, 0);
        uint32 whitetg  = (uint32) strtoul(param2, NULL, 0);
        uint32 greentg  = (uint32) strtoul(param3, NULL, 0);
        uint32 bluetg   = (uint32) strtoul(param4, NULL, 0);
        uint32 purpletg = (uint32) strtoul(param5, NULL, 0);
        uint32 orangetg = (uint32) strtoul(param6, NULL, 0);
        uint32 yellowtg = (uint32) strtoul(param7, NULL, 0);
        uint32 greyi    = (uint32) strtoul(param8, NULL, 0);
        uint32 whitei   = (uint32) strtoul(param9, NULL, 0);
        uint32 greeni   = (uint32) strtoul(param10, NULL, 0);
        uint32 bluei    = (uint32) strtoul(param11, NULL, 0);
        uint32 purplei  = (uint32) strtoul(param12, NULL, 0);
        uint32 orangei  = (uint32) strtoul(param13, NULL, 0);
        uint32 yellowi  = (uint32) strtoul(param14, NULL, 0);

        //
        // Setup the percentage in the configuration first, so validity test can be performed
        //

        config->SetPercentages(greytg, whitetg, greentg, bluetg, purpletg, orangetg, yellowtg, greyi, whitei, greeni, bluei, purplei, orangei, yellowi);

        //
        // Save the results into the database (after the tests)
        //

        auto trans = WorldDatabase.BeginTransaction();

        trans->Append("UPDATE mod_auctionhousebot SET percentgreytradegoods   = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_GREY_TG)  , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentwhitetradegoods  = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_WHITE_TG) , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentgreentradegoods  = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_GREEN_TG) , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentbluetradegoods   = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_BLUE_TG)  , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentpurpletradegoods = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_PURPLE_TG), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentorangetradegoods = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_ORANGE_TG), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentyellowtradegoods = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_YELLOW_TG), ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentgreyitems        = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_GREY_I)   , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentwhiteitems       = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_WHITE_I)  , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentgreenitems       = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_GREEN_I)  , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentblueitems        = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_BLUE_I)   , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentpurpleitems      = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_PURPLE_I) , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentorangeitems      = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_ORANGE_I) , ahMapID);
        trans->Append("UPDATE mod_auctionhousebot SET percentyellowitems      = '{}' WHERE auctionhouse = '{}'", config->GetPercentages(AHB_YELLOW_I) , ahMapID);

        WorldDatabase.CommitTransaction(trans);

        break;
    }
    case AHBotCommand::minprice:
    {
        char * param1   = strtok(args, " ");
        uint32 minPrice = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET minprice{} = '{}' WHERE auctionhouse = '{}'", color, minPrice, ahMapID);

        config->SetMinPrice(col, minPrice);

        break;
    }
    case AHBotCommand::maxprice:
    {
        char * param1   = strtok(args, " ");
        uint32 maxPrice = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET maxprice{} = '{}' WHERE auctionhouse = '{}'", color, maxPrice, ahMapID);

        config->SetMaxPrice(col, maxPrice);

        break;
    }
    case AHBotCommand::minbidprice:
    {
        char * param1      = strtok(args, " ");
        uint32 minBidPrice = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET minbidprice{} = '{}' WHERE auctionhouse = '{}'", color, minBidPrice, ahMapID);

        config->SetMinBidPrice(col, minBidPrice);

        break;
    }
    case AHBotCommand::maxbidprice:
    {
        char * param1      = strtok(args, " ");
        uint32 maxBidPrice = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET maxbidprice{} = '{}' WHERE auctionhouse = '{}'", color, maxBidPrice, ahMapID);

        config->SetMaxBidPrice(col, maxBidPrice);

        break;
    }
    case AHBotCommand::maxstack:
    {
        char * param1   = strtok(args, " ");
        uint32 maxStack = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET maxstack{} = '{}' WHERE auctionhouse = '{}'", color, maxStack, ahMapID);

        config->SetMaxStack(col, maxStack);

        break;
    }
    case AHBotCommand::buyerprice:
    {
        char * param1     = strtok(args, " ");
        uint32 buyerPrice = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET buyerprice{} = '{}' WHERE auctionhouse = '{}'", color, buyerPrice, ahMapID);

        config->SetBuyerPrice(col, buyerPrice);

        break;
    }
    case AHBotCommand::bidinterval:
    {
        char * param1      = strtok(args, " ");
        uint32 bidInterval = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET buyerbiddinginterval = '{}' WHERE auctionhouse = '{}'", bidInterval, ahMapID);

        config->SetBiddingInterval(bidInterval);

        break;
    }
    case AHBotCommand::bidsperinterval:
    {
        char * param1          = strtok(args, " ");
        uint32 bidsPerInterval = (uint32) strtoul(param1, NULL, 0);

        WorldDatabase.Execute("UPDATE mod_auctionhousebot SET buyerbidsperinterval = '{}' WHERE auctionhouse = '{}'", bidsPerInterval, ahMapID);

        config->SetBidsPerInterval(bidsPerInterval);

        break;
    }
    default:
        break;
    }
}

// =============================================================================
// Initialization of the bot
// =============================================================================

void AuctionHouseBot::Initialize(AHBConfig* allianceConfig, AHBConfig* hordeConfig, AHBConfig* neutralConfig)
{
    _allianceConfig = allianceConfig;
    _hordeConfig = hordeConfig;
    _neutralConfig = neutralConfig;

    _allianceConfig->LoadPriceOverrides();
    _hordeConfig->LoadPriceOverrides();
    _neutralConfig->LoadPriceOverrides();
}

// Helper function to join GUIDs into a comma-separated string
std::string JoinGUIDs(const std::vector<uint32>& guids)
{
    std::ostringstream oss;
    for (size_t i = 0; i < guids.size(); ++i)
    {
        if (i != 0)
        {
            oss << ",";
        }
        oss << guids[i];
    }
    return oss.str();
}
