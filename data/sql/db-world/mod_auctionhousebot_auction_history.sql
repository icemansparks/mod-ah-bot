CREATE TABLE `mod_auctionhousebot_auction_history` (
    `id` INT AUTO_INCREMENT PRIMARY KEY,
    `seller` INT NOT NULL,
    `buyer` INT DEFAULT NULL;
    `item_id` INT NOT NULL,
    `quantity` INT NOT NULL,
    `final_price` BIGINT NOT NULL,
    `auction_type` ENUM('bid', 'buyout', 'expired') NOT NULL,
    `timestamp` TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
