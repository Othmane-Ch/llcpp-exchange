Feature: Market Data
  As a market data consumer
  I want to receive accurate market data updates
  So that I can track order book changes and trades in real time

  Background:
    Given a fresh exchange engine

  Scenario: New order generates ADD market update
    When I submit a BUY order for 10 shares at price 100
    Then I should receive 1 market update of type "ADD"
    And the market update should have price 100 and quantity 10

  Scenario: Trade generates TRADE market update
    Given a resting SELL order for 20 shares at price 100 from client 1
    When client 2 submits a BUY order for 20 shares at price 100
    Then I should receive at least 1 market update of type "TRADE"

  Scenario: Cancel generates CANCEL market update
    Given a resting BUY order for 10 shares at price 100 from client 1
    When client 1 cancels order 1 on ticker 0
    Then I should receive 1 market update of type "CANCEL"
    And the cancel update should have quantity 10

  Scenario: Multiple price level sweep generates multiple trade updates
    Given a resting SELL order for 10 shares at price 100 from client 1 with order id 1
    And a resting SELL order for 10 shares at price 101 from client 1 with order id 2
    When client 2 submits a BUY order for 20 shares at price 101
    Then I should receive at least 2 market updates of type "TRADE"
