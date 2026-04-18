/**
 * Simple semantic-version comparator for tags like "v1.2.3" / "1.2.3".
 *
 * Rules (intentionally loose):
 *  - Leading 'v' / 'V' is stripped
 *  - Anything after a '-' or '+' is dropped ("v1.2.3-rc1" → 1.2.3)
 *  - Missing components default to 0 ("1.2" → 1.2.0)
 *  - Non-numeric components compare as 0 to avoid throwing on garbage
 *
 * Returns > 0 if newTag > installed, 0 if equal, < 0 if older.
 *
 * Copyright (c) 2026 Proof of Prints
 */
package com.proofofprints.popmobile.update

internal object VersionCompare {

    fun isNewer(newTag: String, installed: String): Boolean =
        compare(newTag, installed) > 0

    fun compare(a: String, b: String): Int {
        val aParts = parts(a)
        val bParts = parts(b)
        val n = maxOf(aParts.size, bParts.size)
        for (i in 0 until n) {
            val ai = aParts.getOrElse(i) { 0 }
            val bi = bParts.getOrElse(i) { 0 }
            if (ai != bi) return ai - bi
        }
        return 0
    }

    private fun parts(v: String): List<Int> {
        val cleaned = v.trim()
            .removePrefix("v").removePrefix("V")
            .substringBefore('-').substringBefore('+')
        return cleaned.split('.').map { it.toIntOrNull() ?: 0 }
    }
}
