Protocol and renderer logic is ported from MacViewer, commit 237c446b3408c9e7d41d7a7493534274fe0563e7. Its MIT notice is retained in MACVIEWER-LICENSE.txt. The UltraViewer 8bit.dat dictionary is excluded from that license and is not distributed here; supply an existing local asset explicitly.

This application links the system Qt6, OpenSSL, Zstandard and libjpeg-turbo libraries. They retain their respective licenses. Consult distribution packaging notices when redistributing their binaries; this repository does not vendor them.

The bundled public Sectigo Public Server Authentication CA DV R36 intermediate certificate supplies a chain element omitted by some relays. Official source: http://crt.sectigo.com/SectigoPublicServerAuthenticationCADVR36.crt. DER SHA-256: 8c54c334b66ba4e426772af4a3f9136c19a1aec729fdb28c535c07a5a4ef22e0. Valid 2021-03-22 through 2036-03-21; verified against system roots on 2026-10-20. No new root is bundled and partial-chain trust is disabled.
