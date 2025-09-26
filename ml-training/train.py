#!/usr/bin/env python3
"""Train a room-classification model from CatLocator CSV exports."""
from __future__ import annotations

import argparse
import json
import pathlib
from dataclasses import dataclass

import joblib
import numpy as np
import pandas as pd
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import classification_report
from sklearn.model_selection import train_test_split
from sklearn.pipeline import Pipeline
from sklearn.preprocessing import OneHotEncoder, StandardScaler
from sklearn.compose import ColumnTransformer


@dataclass
class TrainingArtifacts:
    model: Pipeline
    feature_columns: list[str]
    label_column: str


def load_dataset(csv_path: pathlib.Path) -> pd.DataFrame:
    df = pd.read_csv(csv_path)
    if 'room' not in df.columns:
        raise ValueError("CSV missing 'room' column")
    return df


def feature_engineering(df: pd.DataFrame) -> tuple[pd.DataFrame, pd.Series]:
    df = df.copy()

    # Basic derived features
    df['beacon_xy_mag'] = np.sqrt(df['beacon_x'] ** 2 + df['beacon_y'] ** 2)

    features = df[['beacon_id', 'tag_id', 'rssi', 'beacon_x', 'beacon_y', 'beacon_z', 'beacon_xy_mag']]
    labels = df['room']
    return features, labels


def build_model() -> Pipeline:
    categorical_features = ['beacon_id', 'tag_id']
    numeric_features = ['rssi', 'beacon_x', 'beacon_y', 'beacon_z', 'beacon_xy_mag']

    preprocessor = ColumnTransformer(
        transformers=[
            ('cat', OneHotEncoder(handle_unknown='ignore'), categorical_features),
            ('num', StandardScaler(), numeric_features),
        ]
    )

    classifier = RandomForestClassifier(n_estimators=200, random_state=42)

    pipeline = Pipeline([
        ('preprocess', preprocessor),
        ('model', classifier),
    ])
    return pipeline


def train(csv_path: pathlib.Path, output_dir: pathlib.Path) -> None:
    print(f"Loading dataset from {csv_path}")
    df = load_dataset(csv_path)
    X, y = feature_engineering(df)

    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=0.2, random_state=42, stratify=y
    )

    print("Training model…")
    pipeline = build_model()
    pipeline.fit(X_train, y_train)

    print("Evaluating…")
    y_pred = pipeline.predict(X_test)
    report = classification_report(y_test, y_pred)
    print(report)

    output_dir.mkdir(parents=True, exist_ok=True)
    model_path = output_dir / 'room_classifier.joblib'
    metadata_path = output_dir / 'metadata.json'

    joblib.dump(pipeline, model_path)
    metadata = {
        'feature_columns': list(X.columns),
        'label_column': 'room',
        'classification_report': report,
    }
    metadata_path.write_text(json.dumps(metadata, indent=2))

    print(f"Saved model to {model_path}")
    print(f"Saved metadata to {metadata_path}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv', type=pathlib.Path, help='CSV file exported from the Go server')
    parser.add_argument('--out', type=pathlib.Path, default=pathlib.Path('artifacts'), help='Output directory')
    args = parser.parse_args()

    train(args.csv, args.out)


if __name__ == '__main__':
    main()
