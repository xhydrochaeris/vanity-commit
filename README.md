# vanity-commit

A tool to create **vanity commit IDs**, by appending a **vanity string** to the commit message.

The current implementation only supports Linux with gcc + OpenMP + libcrypto.

GPU support is NO LONGER planned for future versions by adding a CUDA implementation. I genuinely couldn't care less.

Extending support for Windows and macOS is also no longer planned. I will consider this done forever after making it an AUR package.

## Build (CPU implementation):

`gcc -O3 -fopenmp vanity_cpu.c -o vanity_cpu -lcrypto`

(Ignore the comments about deprecated functions)

## Run:

Test the CPU implementation:

`./vanity_cpu "<commit-content-before-trailer>" "69420"`

Make the commit:

`git add .`

`./vanity-commit.py -m "Initial Commit ( ͡° ͜ʖ ͡°)" "69420"`

This matches the beginning of your commit hash ID with the provided string. And yes, it works. Just look at this commit's ID!!

## Note:

The default branch for this project is called **vain**, not main. This is a deliberate pun.

# Supported arguments and options

These arguments and options are not a complete implementation of Git's `git commit` interface. They behave mostly like their Git counterparts, with some differences. See the [Git commit documentation](https://git-scm.com/docs/git-commit) for the original behavior.

## Commit message sources

These arguments are mutually exclusive.

- `-m`, `--message`: Specify the commit message. If multiple `-m` arguments are specified, they are concatenated with two newlines (`\n\n`). If no message source is specified, your preferred editor configured in Git is opened.
  - Unlike Git, where lines starting with `#` are ignored when editing a commit message, this editor includes everything in the temporary file as part of the commit message.
- `-F`, `--file`: Read the commit message from a file.
- `-C <commit>`: Reuse the commit message, author, and author date from the specified commit. The editor is not opened.
- `-c <commit>`: Like `-C`, but open the editor with the reused commit message prepopulated so you can modify it.

## Other arguments and options

- `-a`, `--all`: Automatically stage files that have been modified and deleted. New files that you have not already told Git about are not affected.
- `--allow-empty`: Allow creating a commit with no changes.
- `--allow-empty-message`: Allow creating a commit with an empty commit message.
  - Note that the commit message won't actually be empty. It will just be the vanity string.
- `--amend`: Replace the tip of the current branch instead of creating a new commit.
  - The current index is used as the new commit's tree.
  - The original commit's parent is retained, effectively replacing the original commit.
  - The original author and author date are retained by default. The committer and committer date are updated.
  - If no message source is specified, the editor opens with the original commit message prepopulated.
- `--author <author>`: Override the author identity. Expected format: `Name <email>`. This does not change the committer identity.
- `--date <date>`: Override the author date. The date must be in a format Git understands. This does not change the committer date.
- `-s`, `--signoff`: Add a signoff trailer using the committer's identity. The final trailer will always be the Vanity string.
- `--trailer <token>[(=|:)<value>]`: Add an arbitrary trailer to the commit message. Can be specified multiple times. These trailers are added before the signoff trailer and the final Vanity trailer.

## Notes on combinations

- `-C` and `-c` are mutually exclusive with each other and with `-m` and `-F`.
- `--amend` can be combined with any of the message sources.
- When `--amend` is used without `-C` or `-c`, the amended commit's author and author date are retained.
- `--author` and `--date` override inherited author metadata from `-C`, `-c`, or `--amend`.
- `-C` and `-c` select the source of the message and author metadata; they do not select the parent commit.
