plom-ii
=======

plomlompom's fork of suckless' ii (<http://tools.suckless.org/ii/>).

Some goals:
- translate slashes in channel names to "," instead of "_" (to allow channel
  names with "_" in them) (DONE)
- show seconds, not only hours:minutes, for each message (DONE)
- promptly remove infiles for parted channels (DONE)
- replace special client commands with raw communication with IRC server (DONE)
  - to do: make input interpretation dependent on which infile is used, i.e.
    allow leaving out channel/user field for those, and, possibly, PRIVMSG
