CREATE SCHEMA auth;

CREATE TABLE auth.accounts (
    player_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    created_at timestamptz NOT NULL DEFAULT CURRENT_TIMESTAMP,
    deleted_at timestamptz
);

CREATE TABLE auth.identities (
    player_id bigint NOT NULL REFERENCES auth.accounts(player_id),
    provider text NOT NULL CHECK (provider <> ''),
    provider_user_id text NOT NULL CHECK (provider_user_id <> ''),
    linked_at timestamptz NOT NULL DEFAULT CURRENT_TIMESTAMP,

    PRIMARY KEY (provider, provider_user_id)
);

CREATE TABLE auth.bans (
    ban_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    player_id bigint NOT NULL REFERENCES auth.accounts(player_id),
    banned_at timestamptz NOT NULL DEFAULT CURRENT_TIMESTAMP,
    banned_until timestamptz,
    ban_reason text,
    revoked_at timestamptz,

    CHECK (banned_until IS NULL OR banned_until > banned_at),
    CHECK (revoked_at IS NULL OR revoked_at >= banned_at)
);

CREATE INDEX identities_player_id_idx
    ON auth.identities(player_id);

CREATE INDEX bans_player_id_idx
    ON auth.bans(player_id);
