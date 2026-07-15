Feature: Order Cancellation
  As a trader using the exchange
  I want to cancel my resting orders
  So that I can manage my risk and positions

  Background:
    Given a fresh exchange engine

  Scenario: Cancel a resting order
    Given a resting BUY order for 10 shares at price 100 from client 1
    When client 1 cancels order 1 on ticker 0
    Then I should receive 1 response of type "CANCELED"
    And I should receive 1 market update of type "CANCEL"
    And the bid side should have 0 levels

  Scenario: Cancel one of multiple orders at same price
    Given a resting BUY order for 10 shares at price 100 from client 1 with order id 1
    And a resting BUY order for 20 shares at price 100 from client 1 with order id 2
    When client 1 cancels order 1 on ticker 0
    Then I should receive 1 response of type "CANCELED"
    And the bid side should have 1 level
    And the bid level at price 100 should have total quantity 20
    And the bid level at price 100 should have 1 orders

  Scenario: Cancel a fully filled order is rejected
    Given a resting SELL order for 10 shares at price 100 from client 1 with order id 1
    And a crossing BUY order for 10 shares at price 100 from client 2 with order id 2
    When client 1 cancels order 1 on ticker 0
    Then I should receive 1 response of type "CANCEL_REJECTED"

  Scenario: Cancel a partially filled order removes remaining quantity
    Given a resting SELL order for 30 shares at price 100 from client 1 with order id 1
    And a crossing BUY order for 10 shares at price 100 from client 2 with order id 2
    When client 1 cancels order 1 on ticker 0
    Then I should receive 1 response of type "CANCELED"
    And I should receive 1 market update of type "CANCEL" with quantity 20
    And the ask side should have 0 levels

  Scenario: Cancel a non-existent order is rejected
    When client 5 cancels order 9999 on ticker 0
    Then I should receive 1 response of type "CANCEL_REJECTED"
    And I should receive 0 market updates
