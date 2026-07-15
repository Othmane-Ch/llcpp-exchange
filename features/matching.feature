Feature: Order Matching
  As a trader using the exchange
  I want crossing orders to be matched correctly
  So that trades execute at the right price and quantity

  Background:
    Given a fresh exchange engine

  Scenario: Buy crosses resting sell for a full match
    Given a resting SELL order for 20 shares at price 100 from client 1
    When client 2 submits a BUY order for 20 shares at price 100
    Then I should receive 1 response of type "ACCEPTED"
    And I should receive 2 fill responses
    And client 2 should be filled for 20 shares with 0 remaining
    And client 1 should be filled for 20 shares with 0 remaining
    And the book should be empty

  Scenario: Sell crosses resting buy for a full match
    Given a resting BUY order for 15 shares at price 100 from client 1
    When client 2 submits a SELL order for 15 shares at price 100
    Then I should receive 2 fill responses
    And the book should be empty

  Scenario: Partial fill leaves remainder in book
    Given a resting SELL order for 60 shares at price 100 from client 1
    When client 2 submits a BUY order for 100 shares at price 100
    Then client 2 should be filled for 60 shares with 40 remaining
    And the bid side should have 1 level
    And the bid level at price 100 should have total quantity 40
    And the ask side should have 0 levels

  Scenario: Aggressive order sweeps multiple resting orders at same price
    Given a resting SELL order for 30 shares at price 100 from client 1 with order id 1
    And a resting SELL order for 30 shares at price 100 from client 1 with order id 2
    And a resting SELL order for 30 shares at price 100 from client 1 with order id 3
    When client 2 submits a BUY order for 100 shares at price 100
    Then I should receive 6 fill responses
    And client 2 total filled quantity should be 90
    And the bid side should have 1 level
    And the bid level at price 100 should have total quantity 10

  Scenario: Aggressive order sweeps multiple price levels
    Given a resting SELL order for 10 shares at price 100 from client 1 with order id 1
    And a resting SELL order for 10 shares at price 101 from client 1 with order id 2
    And a resting SELL order for 10 shares at price 102 from client 1 with order id 3
    When client 2 submits a BUY order for 30 shares at price 102
    Then client 2 total filled quantity should be 30
    And the book should be empty

  Scenario: FIFO priority - first order at same price matches first
    Given a resting SELL order for 5 shares at price 100 from client 1 with order id 1
    And a resting SELL order for 5 shares at price 100 from client 1 with order id 2
    When client 2 submits a BUY order for 5 shares at price 100
    Then the first resting order should be matched
    And the ask side should have 1 level
    And the ask level at price 100 should have total quantity 5
