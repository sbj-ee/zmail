Fixtures made with the real `minisign` 0.12 and a throwaway **test** key
(its secret key was deleted; it signs nothing else). Copied unchanged from
sbj-ee/zterminal's `tests/data/minisign/` (their file names say "zterminal"; the
signature covers those exact bytes, so they are left alone):

    minisign -G -W -p test.pub -s test.key
    minisign -S -s test.key -m SHA256SUMS -t "timestamp:0 file:SHA256SUMS hashed"
    minisign -S -l -s test.key -m SHA256SUMS.legacy -t "legacy test"   # "Ed" (not prehashed)

They prove zmail's verifier reads minisign's own output. Not the release key.
