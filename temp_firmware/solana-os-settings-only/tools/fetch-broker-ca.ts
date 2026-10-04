/**
 * Regenerates src/net/broker_ca.h — the pinned trust anchor compiled into the
 * firmware for the default Let's Encrypt-issued broker (DEFAULT_BROKER_URL).
 *
 * It downloads the two ISRG roots (Let's Encrypt's long-lived trust anchors)
 * and verifies each against a pinned SHA-256 fingerprint BEFORE writing. The
 * pin is the security boundary: even over a MITM'd connection a substituted
 * root fails the fingerprint check and nothing is written, so this is safe to
 * run from any build machine (and `bun run firmware` runs it automatically).
 *
 *   bun run tools/fetch-broker-ca.ts
 *
 * We pin the ROOTS, not the ~90-day leaf, so routine cert rotation on the
 * broker doesn't invalidate a flashed badge. mbedTLS builds the chain from the
 * server-sent intermediate up to whichever of these signed it.
 */

import { createHash } from 'node:crypto';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

type Root = { name: string; url: string; sha256: string };

// SHA-256 fingerprints of the DER — the published, stable ISRG root identities.
const ROOTS: Root[] = [
	{
		name: 'ISRG Root X1',
		url: 'https://letsencrypt.org/certs/isrgrootx1.pem',
		sha256: '96BCEC06264976F37460779ACF28C5A7CFE8A3C0AAE11A8FFCEE05C0BDDF08C6'
	},
	{
		name: 'ISRG Root X2',
		url: 'https://letsencrypt.org/certs/isrg-root-x2.pem',
		sha256: '69729B8E15A86EFC177A57AFB7171DFC64ADD28C2FCA8CF1507E34453CCB1470'
	}
];

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, '..', 'src', 'net', 'broker_ca.h');

/** DER SHA-256 (uppercase hex, no separators) of a single PEM certificate. */
function fingerprint(pem: string): string {
	const b64 = pem
		.replace(/-----BEGIN CERTIFICATE-----/g, '')
		.replace(/-----END CERTIFICATE-----/g, '')
		.replace(/\s+/g, '');
	const der = Buffer.from(b64, 'base64');
	return createHash('sha256').update(der).digest('hex').toUpperCase();
}

function fpColons(hex: string): string {
	return (hex.match(/../g) ?? []).join(':');
}

const pems: string[] = [];
for (const root of ROOTS) {
	const res = await fetch(root.url);
	if (!res.ok) throw new Error(`${root.name}: fetch failed (${res.status}) from ${root.url}`);
	const pem = (await res.text()).trim();
	const got = fingerprint(pem);
	if (got !== root.sha256) {
		// Fail closed — do NOT write. A mismatch means the download was tampered
		// with, or Let's Encrypt rotated a root (update the pin deliberately).
		throw new Error(
			`${root.name}: SHA-256 pin mismatch\n  expected ${root.sha256}\n  got      ${got}\n` +
				`Refusing to write ${OUT}.`
		);
	}
	console.log(`  ${root.name}  ✓ ${fpColons(got)}`);
	pems.push(pem);
}

const header =
	`// GENERATED FILE — do not edit by hand.
//
// The pinned trust anchor for the default Let's Encrypt-issued broker
// (DEFAULT_BROKER_URL). broker_client.cpp uses this when cert_store holds no
// operator-provisioned "broker-ca", so a stock badge validates the broker's TLS
// out of the box instead of failing closed.
//
// These are the ISRG roots (Let's Encrypt's trust anchors), NOT the 90-day leaf
// — pinning the roots (valid into 2035/2040) survives routine cert rotation.
// mbedTLS builds the chain from the server-sent intermediate up to one of these.
//
// Unlike the WPA2-Enterprise CA (cert_store.h explains why that one is uploaded,
// not compiled in: it is a conference RADIUS cert reissued yearly), a public
// ISRG root is long-lived and safe to ship in the image.
//
// Regenerate with:  bun run tools/fetch-broker-ca.ts
// (the app build step \`bun run firmware\` runs this automatically).
//
` +
	ROOTS.map((r) => `//   ${r.name.padEnd(13)} SHA-256 ${fpColons(r.sha256)}`).join('\n') +
	`
#pragma once

// Concatenated ISRG Root X1 (RSA) + X2 (ECDSA), PEM. setCACert() accepts a
// multi-cert bundle, covering both the RSA and ECDSA Let's Encrypt chains.
constexpr char BROKER_DEFAULT_CA_PEM[] = R"CApem(
${pems.join('\n')}
)CApem";
`;

await Bun.write(OUT, header);
console.log(`wrote ${OUT}`);
