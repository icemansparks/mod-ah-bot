/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ScriptMgr.h"
#include "AuctionHouseBot.h"
#include "Log.h"
#include "Mail.h"
#include "Player.h"
#include "WorldSession.h"

class AHBot_WorldScript : public WorldScript
{
public:
    AHBot_WorldScript() : WorldScript("AHBot_WorldScript") { }

    void OnBeforeConfigLoad(bool /*reload*/) override
    {
        sAHBot->InitializeConfiguration();
    }

    void OnStartup() override
    {
        LOG_INFO("server.loading", "Initialize AuctionHouseBot...");
        sAHBot->Initialize();
    }
};

class AHBot_AuctionHouseScript : public AuctionHouseScript
{
public:
    AHBot_AuctionHouseScript() : AuctionHouseScript("AHBot_AuctionHouseScript") { }

    void OnBeforeAuctionHouseMgrSendAuctionSuccessfulMail(AuctionHouseMgr* /*auctionHouseMgr*/, AuctionEntry* /*auction*/, Player* owner, uint32& /*owner_accId*/, uint32& /*profit*/, bool& sendNotification, bool& updateAchievementCriteria, bool& /*sendMail*/) override
    {
        if (owner && owner->GetGUID().GetCounter() == sAHBot->GetAHBplayerGUID())
        {
            sendNotification = false;
            updateAchievementCriteria = false;
        }
    }

    void OnBeforeAuctionHouseMgrSendAuctionExpiredMail(AuctionHouseMgr* /*auctionHouseMgr*/, AuctionEntry* /*auction*/, Player* owner, uint32& /*owner_accId*/, bool& sendNotification, bool& /*sendMail*/) override
    {
        if (owner && owner->GetGUID().GetCounter() == sAHBot->GetAHBplayerGUID())
            sendNotification = false;
    }

    void OnBeforeAuctionHouseMgrSendAuctionOutbiddedMail(AuctionHouseMgr* /*auctionHouseMgr*/, AuctionEntry* auction, Player* oldBidder, uint32& /*oldBidder_accId*/, Player* newBidder, uint32& newPrice, bool& /*sendNotification*/, bool& /*sendMail*/) override
    {
        if (oldBidder && !newBidder)
            oldBidder->GetSession()->SendAuctionBidderNotification((uint32)auction->GetHouseId(), auction->Id, ObjectGuid::Create<HighGuid::Player>(sAHBot->GetAHBplayerGUID()), newPrice, auction->GetAuctionOutBid(), auction->item_template);
    }

    void OnAuctionAdd(AuctionHouseObject* /*ah*/, AuctionEntry* auction) override
    {
        sAHBot->IncrementItemCounts(auction);
    }

    void OnAuctionRemove(AuctionHouseObject* /*ah*/, AuctionEntry* auction) override
    {
        sAHBot->DecrementItemCounts(auction, auction->item_template);
    }

    void OnBeforeAuctionHouseMgrUpdate() override
    {
        sAHBot->Update();
    }
};

class AHBot_MailScript : public MailScript
{
public:
    AHBot_MailScript() : MailScript("AHBot_MailScript") { }

    void OnBeforeMailDraftSendMailTo(MailDraft* /*mailDraft*/, MailReceiver const& receiver, MailSender const& sender, MailCheckMask& /*checked*/, uint32& /*deliver_delay*/, uint32& /*custom_expiration*/, bool& deleteMailItemsFromDB, bool& sendMail) override
    {
        if (receiver.GetPlayerGUIDLow() == sAHBot->GetAHBplayerGUID())
        {
            if (sender.GetMailMessageType() == MAIL_AUCTION)        // auction mail with items
                deleteMailItemsFromDB = true;
            sendMail = false;
        }
    }
};

void AddAHBotScripts()
{
    new AHBot_WorldScript();
    new AHBot_AuctionHouseScript();
    new AHBot_MailScript();
}
