/**
 * Validators for the three settings fields that the miner needs before it
 * can talk to a pool. Each `validate*` returns null when the input is
 * acceptable, or a short human-readable error message otherwise.
 *
 * Intentionally loose — we want to catch clear typos and empty values,
 * not reject edge-case-but-valid input. Example: we don't run a bech32
 * checksum on the Kaspa address, just a structural sanity check.
 *
 * Copyright (c) 2026 Proof of Prints
 */
package com.proofofprints.popmobile.ui

internal object FieldValidators {

    /**
     * stratum+tcp://host:port — host is a DNS name or IPv4, port 1-65535.
     * Trailing slash tolerated.
     */
    private val POOL_URL_REGEX =
        Regex("""^stratum\+tcp://([A-Za-z0-9][A-Za-z0-9\-.]{0,253})(?::([0-9]{1,5}))?/?$""")

    /**
     * Kaspa address: mainnet `kaspa:…` or testnet `kaspatest:…`, followed
     * by 40+ chars of bech32-ish lowercase alphanumeric. Real mainnet
     * payloads are ~61 chars; 40 is a conservative minimum that still
     * catches obvious typos.
     */
    private val KASPA_ADDRESS_REGEX =
        Regex("""^kaspa(?:test)?:[a-z0-9]{40,}$""")

    /** Pool stratum workers commonly reject spaces and most punctuation. */
    private val WORKER_NAME_REGEX = Regex("""^[A-Za-z0-9_\-.]{1,64}$""")

    fun validatePoolUrl(value: String): String? {
        val v = value.trim()
        if (v.isEmpty()) return "Pool URL is required"
        if (!v.startsWith("stratum+tcp://")) return "Must start with stratum+tcp://"
        val match = POOL_URL_REGEX.matchEntire(v)
            ?: return "Expected stratum+tcp://host:port"
        val port = match.groupValues[2].toIntOrNull()
            ?: return "Missing port — expected :port after host"
        if (port !in 1..65535) return "Port must be between 1 and 65535"
        return null
    }

    fun validateWalletAddress(value: String): String? {
        val v = value.trim()
        if (v.isEmpty()) return "Wallet address is required"
        val lower = v.lowercase()
        if (!lower.startsWith("kaspa:") && !lower.startsWith("kaspatest:")) {
            return "Must start with kaspa:"
        }
        if (!KASPA_ADDRESS_REGEX.matches(lower)) return "Address looks malformed"
        return null
    }

    fun validateWorkerName(value: String): String? {
        val v = value.trim()
        if (v.isEmpty()) return "Worker name is required"
        if (!WORKER_NAME_REGEX.matches(v)) return "Only letters, digits, _ - ."
        return null
    }

    /**
     * Returns the first error among the three fields, already prefixed with
     * a short field label so toast messages like "Wallet: Must start with
     * kaspa:" make sense without extra context. Null if all three pass.
     */
    fun firstError(poolUrl: String, wallet: String, worker: String): String? =
        validatePoolUrl(poolUrl)?.let { "Pool: $it" }
            ?: validateWalletAddress(wallet)?.let { "Wallet: $it" }
            ?: validateWorkerName(worker)?.let { "Worker: $it" }
}

/** Default pool URL used when the user hasn't saved their own yet. */
internal const val DEFAULT_POOL_URL = "stratum+tcp://pool.proofofprints.com:5558"
