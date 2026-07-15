Feature: Client-side Market Order Book reconstruction
  As a market participant client
  I want my Trading::MarketOrderBook to reflect the exchange's order book
  So that my strategies can trade against an accurate view of the market

  Background:
    Given a fresh market order book

  Scenario: A single ADD establishes the best bid
    When the book receives an ADD for BUY 10 @ 100 oid 1
    Then the best bid should be 100 with quantity 10
    And the best ask should be absent

  Scenario: Two ADDs at the same price aggregate BBO quantity
    When the book receives an ADD for BUY 10 @ 100 oid 1
    And the book receives an ADD for BUY 5 @ 100 oid 2
    Then the best bid should be 100 with quantity 15
    And the book should have order 1 resting
    And the book should have order 2 resting

  Scenario: MODIFY updates the resting quantity without creating a new order
    When the book receives an ADD for SELL 20 @ 101 oid 7
    And the book receives a MODIFY for SELL 8 @ 101 oid 7
    Then the best ask should be 101 with quantity 8
    And the book should have order 7 resting

  Scenario: CANCEL removes the order and the price level when empty
    When the book receives an ADD for BUY 10 @ 99 oid 3
    And the book receives a CANCEL for BUY 99 oid 3
    Then the best bid should be absent
    And the book should not have order 3 resting

  Scenario: Bids descend and asks ascend by price
    When the book receives an ADD for BUY 5 @ 98 oid 1
    And the book receives an ADD for BUY 5 @ 100 oid 2
    And the book receives an ADD for BUY 5 @ 99 oid 3
    And the book receives an ADD for SELL 5 @ 102 oid 4
    And the book receives an ADD for SELL 5 @ 101 oid 5
    And the book receives an ADD for SELL 5 @ 103 oid 6
    Then the best bid should be 100 with quantity 5
    And the best ask should be 101 with quantity 5
    And the bid levels in order should be 100, 99, 98
    And the ask levels in order should be 101, 102, 103

  Scenario: CLEAR wipes all state
    When the book receives an ADD for BUY 10 @ 100 oid 1
    And the book receives an ADD for SELL 5 @ 101 oid 2
    And the book receives a CLEAR
    Then the best bid should be absent
    And the best ask should be absent
    And the book should not have order 1 resting
    And the book should not have order 2 resting

  Scenario: TRADE is informational and does not mutate the book
    When the book receives an ADD for BUY 10 @ 100 oid 1
    And the book receives a TRADE for SELL 3 @ 100
    Then the best bid should be 100 with quantity 10
