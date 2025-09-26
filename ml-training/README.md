# CatLocator ML Training

This directory contains experimental notebooks/scripts for training a room-classification model from exported telemetry.

## Workflow
1. Export labeled data from the Go server dashboard (`Export Training CSV`).
2. Run `python train.py path/to/catlocator_training.csv`. The script:
   - Loads the CSV into a Pandas DataFrame
   - Engineers features from raw RSSI and beacon metadata
   - Splits the dataset into train/validation sets
   - Trains a RandomForestClassifier (scikit-learn)
   - Serializes the model and preprocessing metadata to `artifacts/` (pickle + JSON)

3. Copy the resulting artifacts to the server/Jetson host for inference integration.

## Requirements
- Python 3.10+
- pip install -r requirements.txt

Jetson Nano with CUDA (optional) can leverage GPU-accelerated libraries later; current script uses CPU-only scikit-learn but isolates feature engineering so it can be swapped for PyTorch/MLX when ready.

## Future Work
- Replace RandomForest with a GPU-friendly model (e.g., PyTorch or MLX) for Jetson deployment.
- Build a Go inference wrapper that loads the exported model.
- Integrate online inference with `/api/location/cat` once the model is validated.
