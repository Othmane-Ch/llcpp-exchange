Feature: Order Book Management
  As a trader using the exchange
  I want my orders to be correctly placed in the order book
  So that they can be matched against incoming orders

  Background:
    Given a fresh exchange engine

  Scenario: Single buy order rests in book
    When I submit a BUY order for 10 shares at price 100
    Then I should receive 1 response of type "ACCEPTED"
    And the bid side should have 1 level
    And the best bid should be 100
    And the ask side should have 0 levels

  Scenario: Single sell order rests in book
    When I submit a SELL order for 5 shares at price 200
    Then I should receive 1 response of type "ACCEPTED"
    And the ask side should have 1 level
    And the best ask should be 200
    And the bid side should have 0 levels

  Scenario: Non-crossing orders populate both sides
    When I submit a BUY order for 10 shares at price 99
    And I submit a SELL order for 5 shares at price 101
    Then the bid side should have 1 level
    And the ask side should have 1 level
    And the best bid should be 99
    And the best ask should be 101
    And the spread should be 2

  Scenario: Multiple orders at same price aggregate quantity
    When I submit a BUY order for 5 shares at price 100
    And I submit a BUY order for 7 shares at price 100
    And I submit a BUY order for 3 shares at price 100
    Then the bid side should have 1 level
    And the bid level at price 100 should have total quantity 15
    And the bid level at price 100 should have 3 orders

  Scenario: Multiple bid price levels are sorted descending
    When I submit a BUY order for 5 shares at price 98
    And I submit a BUY order for 5 shares at price 100
    And I submit a BUY order for 5 shares at price 99
    Then the bid side should have 3 levels
    And bid level 1 should have price 100
    And bid level 2 should have price 99
    And bid level 3 should have price 98

  Scenario: Empty book has no levels or best prices
    Then the bid side should have 0 levels
    And the ask side should have 0 levels
    And the best bid should be empty
    And the best ask should be empty
    And the spread should be empty
