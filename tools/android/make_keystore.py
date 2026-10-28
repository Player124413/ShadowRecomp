#!/usr/bin/env python3
"""The APK's signing key, so a new build installs over the old one.

Android refuses to update an installed app with an APK signed by a different key: the installer says "App not installed"
and the only way forward is to uninstall, which wipes the settings, the saves and the imported disc image. The CI build
used Android's *debug* key, which Gradle generates fresh on every machine it builds on - so every run produced an APK
with a new key and every update cost the player their data.

This writes one fixed key and commits it (android/shadow-key.p12 + its password), so every build - CI or on a PC - is
signed identically and `versionCode` alone decides whether it is an update.

It is not a secret and must not be treated as one: it cannot impersonate any other app, cannot be published anywhere as
anyone else, and protects nothing (there is no store listing and no purchase). It only makes the *same* app updatable.
Run it again only if the key is ever lost - and then every install needs one more uninstall.

    python3 tools/android/make_keystore.py            # writes android/shadow-key.p12 if it does not exist
    python3 tools/android/make_keystore.py --force     # replaces it (every install becomes a new app)
"""
import argparse
import datetime
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, 'android', 'shadow-key.p12')
PASSWORD = 'shadowrecomp'
ALIAS = 'shadowrecomp'


def main():
    ap = argparse.ArgumentParser(description='Write the port\'s fixed APK signing key.')
    ap.add_argument('--force', action='store_true', help='replace the existing key (all installs become new apps)')
    args = ap.parse_args()

    if os.path.exists(OUT) and not args.force:
        print(f'{OUT} already exists - nothing to do (use --force to replace it, which breaks updates)')
        return 0

    try:
        from cryptography import x509
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import rsa
        from cryptography.hazmat.primitives.serialization import pkcs12
        from cryptography.x509.oid import NameOID
    except ImportError:
        print('needs the "cryptography" package: python3 -m pip install cryptography', file=sys.stderr)
        return 1

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([
        x509.NameAttribute(NameOID.COMMON_NAME, 'ShadowRecomp'),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, 'ShadowRecomp (Android port)'),
    ])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder()
            .subject_name(name)
            .issuer_name(name)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            # Android checks that the certificate is valid at install time; the end date only has to be far away.
            .not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=365 * 30))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(x509.KeyUsage(digital_signature=True, content_commitment=False, key_encipherment=False,
                                         data_encipherment=False, key_agreement=False, key_cert_sign=False,
                                         crl_sign=False, encipher_only=False, decipher_only=False), critical=True)
            .sign(key, hashes.SHA256()))

    blob = pkcs12.serialize_key_and_certificates(
        name=ALIAS.encode(),
        key=key,
        cert=cert,
        cas=None,
        encryption_algorithm=serialization.BestAvailableEncryption(PASSWORD.encode()))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, 'wb') as f:
        f.write(blob)
    print(f'wrote {OUT} ({len(blob)} bytes), alias {ALIAS}, password "{PASSWORD}"')
    return 0


if __name__ == '__main__':
    sys.exit(main())
