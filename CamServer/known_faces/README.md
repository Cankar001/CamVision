# Known faces

Put the photos of the people, who should be recognized, into this folder: **one folder per person**, named like the person, with one or more photos of that person in it.

```
known_faces/
  Alice/
    1.jpg
    2.jpg
  Bob/
    portrait.png
```

- Photos directly in this folder work too, the file name is the name of the person (`Carol.jpg`).
- Use clear photos, in which the face is well visible and looks into the camera. Several photos of the same person (different light, with and without glasses) make the recognition more reliable.
- A photo, in which no face is found, is skipped (a warning is logged). If a photo shows several faces, the biggest one is used.
- The server notices added, changed or removed photos by itself, a restart is not needed.
- The photos are personal data. This folder is not part of the repository (see `.gitignore`).
