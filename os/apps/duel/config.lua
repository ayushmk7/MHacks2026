-- Duel: everything a person might want to change. Edit and push again.
-- Both badges should run the same values; the inviter's stake is the one that counts.
return {
  -- Stakes offered on the title screen (at most 6 fit). Strings in display units, written with
  -- the token's decimals; never numbers.
  stakes = {"1.00", "5.00", "10.00", "25.00"},
  stake_default = 1,          -- index into stakes selected when the app starts
  -- symbol = "HACK",         -- default: the first token of the provisioned table

  rounds = 3,                 -- best of: the first to win rounds // 2 + 1 rounds wins the duel
  max_rounds = 9,             -- a tied round is replayed; after this many rounds it is a draw

  delay_min_ms = 2000,        -- the wait before the flash, chosen by the inviter each round
  delay_max_ms = 5000,
  reaction_max_ms = 3000,     -- no press within this long counts as this time
  round_pause_ms = 2500,      -- how long a round's result stays up

  invite_every_ms = 500,      -- INVITE is broadcast this often while inviting
  resend_ms = 400,            -- ACCEPT, GO and TIME are sent again this often until answered

  invite_timeout_s = 15,      -- inviting with nobody answering: back to the title
  offer_timeout_s = 15,       -- an invitation left unanswered: back to the title
  start_timeout_s = 8,        -- accepted, but the first round never started
  round_timeout_s = 10,       -- the other badge went quiet during a round
  settle_timeout_s = 90,      -- winner: "unpaid" without a confirmed payment within this long;
                              -- loser: gives up looking for the winner's request
  pay_timeout_s = 60,         -- loser: one step of the payment made no progress for this long
  result_timeout_s = 20,      -- the final screen: back to the title

  find_every_ms = 500,        -- loser: how often wallet.requests() is searched
  confirm_every_s = 2,        -- winner: how often the payment is looked up on chain

  flash_led_ms = 400,         -- LED pulse at the flash
  result_led_ms = 1200,       -- LED pulse when the stake is paid
}
