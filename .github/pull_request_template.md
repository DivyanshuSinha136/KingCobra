## Summary

<!-- What does this change, and why? Link the issue it closes: "Closes #123". -->

## Type of change

- [ ] Bug fix (no documented behavior changes)
- [ ] New feature / new std module (additive)
- [ ] Behavior change (needs a **Changed** entry with a migration note — see docs/STABILITY.md)
- [ ] Docs / examples / tests only
- [ ] Build, CI or packaging

## Area

<!-- compiler · std.<module> · kcc · kasm · kcos · kcide · docs · packaging -->

## How was it tested?

<!-- Commands you ran and their result. -->

```text
make test
./kingcobra test
```

## Checklist

- [ ] `make test` passes locally
- [ ] Added or updated tests (`tests/` or `examples/`)
- [ ] Added or updated docs (`docs/`) for user-visible changes
- [ ] Added a line under **Unreleased** in `docs/CHANGELOG.md`
- [ ] No new third-party build-time dependency
- [ ] Platform-specific code goes through `compiler/include/kc_platform.h`
- [ ] Ran with `--leaks` where the change touches memory

## Notes for reviewers

<!-- Anything tricky, trade-offs, follow-ups, or platforms you could not test. -->
