"""The one-badge half of the contacts tests (WP34): the badge accepts a card it made for its own
HELLO. Checks the contact store, the replay refusal, the nonce rotation, the rename over an
existing file on LittleFS, and that the contact survives a reset. T-CON1 and T-CON2 (t_con.py)
need two badges."""

from t_con import single


def run(badge):
    single(badge)
